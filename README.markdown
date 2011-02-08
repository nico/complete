complete
========

This package contains a set of tools you can use when you build your project
with clang, such as:

* A clang plugin that dumps type information clang sees into a sqlite database.
* `builddb-cc`, a cc replacement script that automatically calls this plugin,
  and that also stores the compiler flags (`-I`, `-D`, etc) and the compilation
  time for every file it builds.
* `tags.py`, a python script that converts the sqlite database into a tags file
  that's compatible with the
  [ctags output format](http://ctags.sourceforge.net/FORMAT) and can be used
  by vim and other editors.
* A web interface to the sqlite database that does filename completion.


Some assembly required
----------------------

Building this is more complicated than it could be -- follow the
"Build like this" steps in `server/complete_plugin.cc` and then follow the
comments at the top of `server/builddb-cc`.

The tools do not support incremental rebuilding of the database, so you always
have to do a clean build of your project when you use `builddb-cc` for now.


Vim integration
---------------

If you use [this fork](https://github.com/nico/clang_complete/tree/perfile) of
the [clang_complete vim plugin](http://www.vim.org/scripts/script.php?script_id=3302),
you get correct autocompletion in all your files if you put something like this
in your `.vimrc`:

    let g:clang_library_path = '/Users/thakis/src/llvm-rw/Release+Asserts/lib'
    let g:clang_use_library = 1

    let g:cr_root = '/Users/thakis/src/chrome-git/src/'
    let g:cr_builddb = '/Users/thakis/src/chrome-git/src/all-builddb.sqlite'

    " Return the compile flags that should be used for file |path| by
    " querying the build database.
    fu! g:clang_per_file_user_options(path)
      if a:path !~? g:cr_root
        return ''
      endif

      let l:path = a:path[strlen(g:cr_root):]
      let l:cmd = 'sqlite3 ' . g:cr_builddb .
          \' ''' . 'select cwd,command from gcc_build_commands join filenames ' .
          \        'on filename_input_id = filenames.rowid where name = "' .
          \        l:path .
          \'"'''
      let l:results = split(system(l:cmd), '|')
      let l:cwd = g:cr_root . l:results[0]
      let l:flags = l:results[1]

      if l:cwd == '' || l:flags == ''
        echo 'Could not find flags or cwd for file '.l:path
        return {}
      endif

      " Filter out options that -cc1 doesn't understand.
      let l:all_flags_list = split(l:flags)
      let l:cc1_flags = []
      let l:i = 0
      let l:e = len(l:all_flags_list)
      while l:i < l:e
        let arg = l:all_flags_list[i]
        if arg =~# "^-[IDFfmOW]"
          call add(l:cc1_flags, arg)
        endif

        if arg == '-isysroot'
          call add(l:cc1_flags, arg)
          call add(l:cc1_flags, l:all_flags_list[i + 1])
          let i += 1
        endif
        if arg == '-arch'
          call add(l:cc1_flags, arg)
          call add(l:cc1_flags, l:all_flags_list[i + 1])
          let i += 1
        endif

        let l:i += 1
      endwhile

      return { 'flags': ' '.join(l:cc1_flags), 'cwd': l:cwd }
    endfu

