add_languages('cpp', native: false)
cpp = meson.get_compiler('cpp')

sources += meson.current_source_dir() / 'utilxx.cc'
if win32
    dependencies += cpp.find_library('runtimeobject')
else 
    dependencies += cpp.find_library('stdc++')
endif
