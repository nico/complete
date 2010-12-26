import cgi
import json
import os
import re
import sqlite3


DB = '/Users/thakis/src/chrome-git/src/builddb.sqlite'

def Score(filename, query):
  pattern = ''
  for q in query:
    pattern += q + '.*'

  m = re.match(pattern, os.path.basename(filename))
  if m:
    return 5
  return 0


def GetResults(filenames, query):
  scored = [(Score(f, query), f) for f in filenames]
  scored.sort(reverse=True)
  return [{ 'path': f } for s, f in scored[0:20]]


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
