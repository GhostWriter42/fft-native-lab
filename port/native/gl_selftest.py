#!/usr/bin/env python3
"""Does an OpenGL 3.3 context open on this machine? (glfw window hidden, moderngl context, one integer texture round trip -- the VRAM texture the GL renderer will use.)
Run with the project venv:  port\\build\\venv\\Scripts\\python.exe port\\native\\gl_selftest.py"""
import sys

import glfw
import moderngl
import numpy as np

if not glfw.init():
    sys.exit('glfw.init failed')
glfw.window_hint(glfw.VISIBLE, glfw.FALSE)
glfw.window_hint(glfw.CONTEXT_VERSION_MAJOR, 3)
glfw.window_hint(glfw.CONTEXT_VERSION_MINOR, 3)
glfw.window_hint(glfw.OPENGL_PROFILE, glfw.OPENGL_CORE_PROFILE)
win = glfw.create_window(64, 64, 'selftest', None, None)
if not win:
    sys.exit('create_window failed')
glfw.make_context_current(win)
ctx = moderngl.create_context()
print('renderer:', ctx.info['GL_RENDERER'])
print('version :', ctx.info['GL_VERSION'])
print('max texture size:', ctx.info['GL_MAX_TEXTURE_SIZE'])

# the PS1 VRAM as an integer texture: 1024 x 512 x 16 bit, written from the CPU and read back
data = (np.arange(1024 * 512, dtype=np.uint32) * 2654435761 % 65536).astype(np.uint16)
tex = ctx.texture((1024, 512), 1, data.tobytes(), dtype='u2')
back = np.frombuffer(tex.read(), dtype=np.uint16)
print('R16UI VRAM texture round trip:', 'OK' if np.array_equal(back, data) else 'MISMATCH')

# a render target 4x the VRAM size (the HD framebuffer): 4096 x 2048 RGBA8
fbo = ctx.framebuffer(ctx.texture((4096, 2048), 4))
print('4x HD framebuffer:', fbo.size, 'complete')
glfw.terminate()
