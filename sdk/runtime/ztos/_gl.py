# SPDX-License-Identifier: GPL-2.0-or-later
"""The GPU layer: what moves is drawn by the GPU, as the phone's own apps do it.

Qt draws widgets on the CPU into the window's buffer and hands the whole buffer
to the shell for every frame: at 1440x2560 that is more than the phone's small
cores can do sixty times a second. So while a list scrolls, tabs slide or a
page turns, a window of our own (a Wayland subsurface, over the app) shows
pictures of the pages, made beforehand, and only says where each one goes;
the widgets under it are left alone until it is over.

A quad is (texture, source rect 0..1, four corners, alpha). The corners are
homogeneous (x, y, w) in points of the layer, so a page turned in perspective
is one quad, drawn exactly by the GPU.
"""
from __future__ import annotations

import struct
from typing import Callable, Optional

import sys

from PySide6.QtCore import QRect, QRectF, Qt, QTimer
from PySide6.QtGui import QImage, QSurfaceFormat, QWindow
from PySide6.QtOpenGL import (QOpenGLBuffer, QOpenGLShader, QOpenGLShaderProgram, QOpenGLTexture,
                              QOpenGLWindow)
from PySide6.QtWidgets import QWidget

GL_TRIANGLES = 0x0004
GL_FLOAT = 0x1406
GL_BLEND = 0x0BE2
GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE = 0x0302, 0x0303, 1
GL_COLOR_BUFFER_BIT = 0x4000
GL_MAX_TEXTURE_SIZE = 0x0D33

VERTEX = """
attribute highp vec4 pos;
attribute highp vec2 uv;
varying highp vec2 v_uv;
void main() { gl_Position = pos; v_uv = uv; }
"""
FRAGMENT = """
#ifdef GL_ES
precision mediump float;
#endif
varying highp vec2 v_uv;
uniform sampler2D tex;
uniform float alpha;
void main() { gl_FragColor = texture2D(tex, v_uv) * alpha; }   /* premultiplied */
"""


class Quad:
    __slots__ = ("key", "src", "corners", "alpha")

    def __init__(self, key, src: QRectF, corners, alpha: float = 1.0):
        self.key, self.src, self.corners, self.alpha = key, src, corners, alpha

    @staticmethod
    def rect(key, src: QRectF, x: float, y: float, w: float, h: float, alpha: float = 1.0) -> "Quad":
        return Quad(key, src, ((x, y, 1.0), (x + w, y, 1.0), (x, y + h, 1.0), (x + w, y + h, 1.0)), alpha)


class _Surface(QOpenGLWindow):
    def __init__(self, layer: "Layer"):
        super().__init__(QOpenGLWindow.NoPartialUpdate)
        self.layer = layer
        self.setFlags(Qt.FramelessWindowHint | Qt.WindowTransparentForInput | Qt.WindowDoesNotAcceptFocus)
        fmt = QSurfaceFormat.defaultFormat()
        fmt.setAlphaBufferSize(0)
        fmt.setSwapInterval(1)
        self.setFormat(fmt)
        self.program: Optional[QOpenGLShaderProgram] = None
        self.vbo: Optional[QOpenGLBuffer] = None
        self.textures: dict = {}
        self.frameSwapped.connect(self._next)

    def initializeGL(self):
        self.program = QOpenGLShaderProgram(self)
        ok = self.program.addShaderFromSourceCode(QOpenGLShader.Vertex, VERTEX) and \
            self.program.addShaderFromSourceCode(QOpenGLShader.Fragment, FRAGMENT)
        self.program.bindAttributeLocation("pos", 0)
        self.program.bindAttributeLocation("uv", 1)
        if not ok or not self.program.link():
            self.layer.broken = True
            print("ztos: GPU layer shaders failed:", self.program.log(), file=sys.stderr)
            return
        self.vbo = QOpenGLBuffer(QOpenGLBuffer.VertexBuffer)
        self.vbo.create()
        self.vbo.setUsagePattern(QOpenGLBuffer.StreamDraw)
        try:
            # as the toolkit: no list picture over 8192 pixels, whatever the GPU allows
            self.layer.max_texture = min(8192, int(self.context().functions().glGetIntegerv(GL_MAX_TEXTURE_SIZE)))
        except Exception:
            self.layer.max_texture = 4096
        self.layer.initialized = True

    drawing = False

    def paintGL(self):
        self.drawing = True     # until the frame is swapped
        f = self.context().functions()
        f.glClearColor(0, 0, 0, 1)
        f.glClear(GL_COLOR_BUFFER_BIT)
        if self.program is None or self.layer.broken or self.layer.frame is None:
            return
        if self.layer.warming:
            self.layer.warming = False
            QTimer.singleShot(0, self.layer.container.hide)
            return
        quads = self.layer.frame()
        if not quads:
            return
        w, h = float(self.width()), float(self.height())
        self.program.bind()
        f.glEnable(GL_BLEND)
        f.glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)   # premultiplied: alpha scales all four
        self.program.setUniformValue1i(self.program.uniformLocation("tex"), 0)
        alpha_at = self.program.uniformLocation("alpha")
        # every quad's six corners in one array, sent once a frame
        drawn, data = [], []
        for q in quads:
            texture = self.textures.get(q.key)
            if texture is None or q.alpha <= 0.01:
                continue
            u0, v0, u1, v1 = q.src.left(), q.src.top(), q.src.right(), q.src.bottom()
            c = q.corners
            # points (homogeneous) to clip space; the GPU divides, exactly
            v = [(2 * x / w - ww, ww - 2 * y / h, 0.0, ww) for x, y, ww in c]
            data += (*v[0], u0, v0, *v[1], u1, v0, *v[2], u0, v1,
                     *v[2], u0, v1, *v[1], u1, v0, *v[3], u1, v1)
            drawn.append((texture, min(1.0, q.alpha)))
        if not drawn:
            self.program.release()
            return
        raw = struct.pack(f"{len(data)}f", *data)
        self.vbo.bind()
        self.vbo.allocate(raw, len(raw))
        self.program.enableAttributeArray(0)
        self.program.enableAttributeArray(1)
        self.program.setAttributeBuffer(0, GL_FLOAT, 0, 4, 24)
        self.program.setAttributeBuffer(1, GL_FLOAT, 16, 2, 24)
        for n, (texture, alpha) in enumerate(drawn):
            self.program.setUniformValue1f(alpha_at, alpha)
            texture.bind(0)
            f.glDrawArrays(GL_TRIANGLES, n * 6, 6)
        self.vbo.release()
        self.program.release()

    def _next(self):
        self.drawing = False
        # A Wayland subsurface (which this is) is synchronised with its parent:
        # its new frame is put on the screen only when the window under it
        # commits too. So the window commits with each frame of ours: one
        # pixel of it repainted, under this layer, nothing to see.
        if self.layer.running:
            self.layer.host.update(0, 0, 1, 1)
        # a frame only when something asks for one (a finger moved, a spring
        # stepped): a finger resting mid-drag costs nothing
        if self.layer.running and self.layer.pending:
            self.layer.pending = False
            self.update()


class Layer:
    """One per app window. show(rect, frame) puts it over that part of the
    window and draws frame() (a list of Quads) every display frame until hide()."""

    def __init__(self, host: QWidget):
        self.host = host
        self.broken = False
        self.running = False
        self.frame: Optional[Callable[[], list]] = None
        self.max_texture = 4096
        self.surface = _Surface(self)
        self.container = QWidget.createWindowContainer(self.surface, host)
        self.container.setAttribute(Qt.WA_TransparentForMouseEvents)
        self.container.setFocusPolicy(Qt.NoFocus)
        self.container.hide()
        self.initialized = False
        self.warming = False
        self.pending = False

    def warm(self):
        """Make the GPU side now (Qt makes it when the surface first shows):
        one black pixel, in the app's own black corner, for a frame."""
        if self.initialized or self.broken:
            return
        self.warming = True
        self.container.setGeometry(QRect(0, 0, 1, 1))
        self.container.show()
        self.surface.update()

    def upload(self, key, image: QImage):
        """A picture for the quads to show, by key (replacing one there)."""
        if not self.initialized:
            return
        self.surface.makeCurrent()
        old = self.surface.textures.pop(key, None)
        if old is not None:
            old.destroy()
        texture = QOpenGLTexture(image.convertToFormat(QImage.Format_RGBA8888_Premultiplied), QOpenGLTexture.DontGenerateMipMaps)
        texture.setMinificationFilter(QOpenGLTexture.Linear)
        texture.setMagnificationFilter(QOpenGLTexture.Linear)
        texture.setWrapMode(QOpenGLTexture.ClampToEdge)
        self.surface.textures[key] = texture
        self.surface.doneCurrent()

    def drop(self, key):
        texture = self.surface.textures.pop(key, None)
        if texture is not None and self.initialized:
            self.surface.makeCurrent()
            texture.destroy()
            self.surface.doneCurrent()

    def has(self, key) -> bool:
        return key in self.surface.textures

    def show(self, rect: QRect, frame: Callable[[], list]):
        from . import _settle
        _settle()               # Qt repaints what is under it: not a change of the page
        self.frame = frame
        self.running = True
        self.container.setGeometry(rect)
        self.container.show()
        self.container.raise_()
        self.surface.update()
        # a subsurface (shown again, or new) appears when its parent commits:
        # the window commits now, and with every frame asked for (update)
        self.host.update(0, 0, 1, 1)

    def update(self):
        """Something moved: draw a frame (one per display frame at most)."""
        if self.running:
            if self.surface.drawing:
                self.pending = True     # after the frame on its way
            else:
                self.surface.update()
            self.host.update(0, 0, 1, 1)    # (the parent commits too: see show)

    def hide(self):
        from . import _settle
        _settle()               # nor is what it uncovers
        self.running = False
        self.frame = None
        self.container.hide()
