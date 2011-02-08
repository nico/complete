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
* Hooks to give you correct autocompletion in vim.


Some assembly required
----------------------

This assumes you've checked out and built clang.

1. Build the plugin. On OS X:

        export LLVM_ROOT=$HOME/src/llvm-rw
        g++ -c complete_plugin.cc \
            `$LLVM_ROOT/Release+Asserts/bin/llvm-config --cxxflags` \
            -I$LLVM_ROOT/tools/clang/include

        g++ -dynamiclib -Wl,-undefined,dynamic_lookup \
            -lsqlite3 complete_plugin.o -o libcomplete_plugin.dylib

2. Build your project with builddb-cc to create the database:

        cd myproject

        # Required.
        export SOURCE_ROOT=$(pwd)

        # Optional, defaults to "clang++'.
        # If clang++ is not in your $PATH, make this absolute.
        export CLANG_CC=$LLVM_ROOT/Release+Asserts/bin/clang++

        # Optional, defaults to "builddb.sqlite".
        # If relative, it's relative to $SOURCE_ROOT.
        export BUILDDB=builddb.sqlite

        # This tool does not support incremental rebuils, so rebuild all.
        rm -rf clang/ && rm builddb.sqlite

        xcodebuild \
            OBJROOT=$SOURCE_ROOT/clang/obj \
            DSTROOT=$SOURCE_ROOT/clang \
            SYMROOT=$SOURCE_ROOT/clang \
            CC=/path/to/complete/server/builddb-cc 

Now you can do several things with this database;

* Create a tags file:

        cd $SOURCE_ROOT
        /path/to/complete/server/tags.py ${BUILDDB:-builddb.sqlite} > tags

* View it in your browser:

        # Start server
        cd server
        python serve_complete.py ~/builddb.sqlite

        # In another terminal, open client
        cd client
        java -jar ~/src/closure/plovr.jar build config.js > complete-compiled.js
        open complete.html


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

