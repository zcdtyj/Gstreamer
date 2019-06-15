project('gl-headers', 'c',
  version : '2019.1.0',
  meson_version : '>= 0.46.0',
  default_options : [ 'warning_level=1',
                      'buildtype=debugoptimized' ])

cc = meson.get_compiler('c')
py3 = import('python').find_installation()

# path that doesn't include any headers aka /dev/null
abyss = include_directories('abyss')

wglext_include = abyss
glext_include = abyss
khrplatform_include = abyss

if not get_option('glext').disabled() # or gles2/3/3.1/etc
  if not cc.has_header('KHR/khrplatform.h') or get_option('glext').enabled() # or gles2/3/3.1/etc
    # header from 'https://www.khronos.org/registry/EGL/api/KHR/khrplatform.h'
    install_headers('khrplatform/KHR/khrplatform.h', subdir : 'KHR')
    khrplatform_include = include_directories('khrplatform')
  endif
endif
if not get_option('wglext').disabled()
  if not cc.has_header('GL/wglext.h') or get_option('wglext').enabled()
    # header from 'https://www.khronos.org/registry/OpenGL/api/GL/wglext.h'
    install_headers('wglext/GL/wglext.h', subdir : 'GL')
    wglext_include = [include_directories('wglext')]
  endif
endif
if not get_option('glext').disabled()
  if not cc.has_header('GL/glext.h') or get_option('glext').enabled()
    # header from 'https://www.khronos.org/registry/OpenGL/api/GL/glext.h'
    install_headers('glext/GL/glext.h', subdir : 'GL')
    glext_include = [include_directories('glext'), khrplatform_include]
  endif
endif

compatibility_includes = [glext_include, wglext_include]

gl_headers_dummy_dep = declare_dependency()
