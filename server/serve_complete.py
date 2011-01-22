import cgi
import json
import os
import re
import sqlite3


DB = '/Users/thakis/src/chrome-git/src/builddb.sqlite'


def GroupsToRanges(m):
  result = []
  for i in xrange(1, 1 + len(m.groups())):
    b, e = m.span(i)
    if result and result[-1][1] + 1 == b:
      result[-1][1] = e - 1
    else:
      result.append([b, e - 1])
  return result


def IsBeginning(index, s):
  """Assumes that index is a valid index into s."""
  return (index > 0 and s[index - 1] in ['_', '/']) or s[index].isupper()


def Score(filename, query):
  # TODO(thakis): After playing with this for a while, it seems as if query
  # characters that don't match at the beginning of a word or right-adjacent to
  # an existing match are less useful.
  pattern = ''
  for q in query:
    # Non-greedy so that '.m' matches the consecutive '.m' in '.mm'.
    pattern += '(%s).*?' % re.escape(q)

  # FIXME: "sptr" should match "scoped_ptr" like "[s]coped_[ptr}". Currently, it
  #        matches like "[s]co[p]ed_p[tr]"

  # FIXME(thakis): maybe don't use just basename. also, probably search()
  # instead of match(). Both seems to work reasonably well in practice though.
  m = re.match(pattern, os.path.basename(filename), re.IGNORECASE)
  if m:
    offset = len(os.path.dirname(filename)) + 1
    ranges = [[b + offset, e + offset] for b, e in GroupsToRanges(m)]

    # Default score for matching at all.
    score = 5

    for r in ranges:
      # Bonus points for every match that starts at the "beginning" of a word.
      # (beginning is after /, after _, and at capital letters).
      if IsBeginning(r[0], filename):
        score += 10

      # Also bonus points for consecutive matches
      score += (r[1] - r[0]) * 11

    # Penalize _test files by giving a small bonus to shorter paths.
    score += 1.0 / len(filename)

    return score, ranges
  return 0, []


def GetResults(filenames, query):
  # This is optimized for hackability, not performance. Could be k log n instead
  # of n, and the complexity in |len(query)| could probably be improved as well.
  scored = [(Score(f, query), f) for f in filenames]
  scored.sort(reverse=True)
  return [{
    'path': f,
    'path_highlight_ranges': s[1],
  } for s, f in scored[0:20] if s[0] > 0]


def serve_search(environ, start_response):
  # TODO(thakis): Should probably be stateful somehow (remember popular files
  #               etc)

  global filenames
  # The closure rich text remote autocomplete control gets data in a whacky
  # format.
  results = [
    ['filenames', {
       'path': 'no',
     }, {
       'path': 'input',
       'path_highlight_ranges': [[0, 1], [3, 4]],
     }, {
       'path': 'yet',
     }],
  ]
  if 'QUERY_STRING' in environ:
    query_dict = cgi.parse_qs(environ['QUERY_STRING'])
    if 'token' in query_dict:
      # parse_qs returns a list for values as query parameters can appear
      # several times (e.g. 'q=ddsview&q=makeicns'). Ignore all but the first
      # occurence of token.
      query = query_dict['token'][0]
      results = [ ['filenames'] + GetResults(filenames, query) ]

  start_response('200 OK',
                [('Content-type','application/json'),
                 # Without this, chrome won't load the json when it's loaded
                 # from a file:// or from a local webserver on a different port
                 ('Access-Control-Allow-Origin', '*')])
  return json.dumps(results).encode('utf-8')


if __name__ == '__main__':
  from wsgiref import simple_server
  global filenames
  # TODO(thakis): Maybe use `git list-files` instead?
  with sqlite3.connect(DB) as db:
    cursor = db.execute('''
        select name from filenames
        ''')
    filenames = map(lambda x: x[0], cursor)

  httpd = simple_server.make_server('', 8080, serve_search)
  httpd.serve_forever()
