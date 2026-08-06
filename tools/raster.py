#!/usr/bin/env python3
"""A small z-buffer rasterizer, because sorting faces was lying to me.

The previous viewer drew triangles back-to-front by their average depth. That is
fine for one closed surface and wrong for exactly the case this project cares
about: two garments touching. Where surfaces interleave, the per-face order
flips from triangle to triangle and the image fills with shards that look like
one mesh poking through another - whether or not it actually does. I read those
shards as a defect once already; the metric happened to agree, but the picture
was not evidence.

A depth buffer resolves occlusion per PIXEL, so what shows up on top is what is
actually in front. Then a shard in the image means a shard in the geometry.

Orthographic on purpose: it is what the OutfitStudio viewport approximates at
low FOV, and it keeps silhouettes comparable between two renders.
"""
from __future__ import annotations
import numpy as np

# right, forward. Z is up in mesh space; Skyrim has front = +y, back = -y.
VIEWS = {
    "frente":   (np.array([1.0, 0, 0]),  np.array([0, -1.0, 0])),
    "costas":   (np.array([-1.0, 0, 0]), np.array([0, 1.0, 0])),
    "perfil":   (np.array([0, -1.0, 0]), np.array([-1.0, 0, 0])),
    "perfilD":  (np.array([0, 1.0, 0]),  np.array([1.0, 0, 0])),
    # costas34 olhava para a FRENTE: fwd tinha y NEGATIVO, o que aponta a camera
    # para +y->-y, e a vista saia sendo uma frontal de tres-quartos com nome de
    # costas. Frente = +y e costas = -y, entao ver as COSTAS exige a camera atras
    # (-y) olhando para +y, isto e fwd.y POSITIVO. O usuario viu; o teste nao,
    # porque testava a geometria da base e nunca o que o nome promete.
    "costas34": (np.array([0.7071, 0.7071, 0]), np.array([-0.7071, 0.7071, 0])),
    "frente34": (np.array([0.7071, -0.7071, 0]),  np.array([-0.7071, -0.7071, 0])),
    "debaixo":  (np.array([1.0, 0, 0]),  np.array([0, -0.45, 0.89])),
}


def view_basis(view):
    """Base ORTONORMAL da camera a partir de (right, fwd).

    `up` era a constante (0,0,1) em todo lugar, o que so funciona quando fwd e
    horizontal. `debaixo` tem fwd=(0,-0.45,0.89): up nao e perpendicular a ele,
    a projecao sai enviesada e a profundidade deixa de corresponder a posicao em
    tela. O teste de instrumento pegou - esfera e toro, ambos fechados por
    construcao, perdiam ~28% da silhueta so nessa vista.

    Gram-Schmidt sobre o par que a tabela ja fornece: fwd manda, right e
    ortogonalizado contra ele, e up sai do produto vetorial. Para toda vista
    horizontal isso devolve exatamente (0,0,1) e nada muda.
    """
    right, fwd = VIEWS[view]
    f = fwd / max(np.linalg.norm(fwd), 1e-12)
    r = right - f * float(np.dot(right, f))
    nr = np.linalg.norm(r)
    if nr < 1e-9:                      # right paralelo a fwd: escolha qualquer
        r = np.cross(f, np.array([0.0, 0.0, 1.0]))
        nr = np.linalg.norm(r)
        if nr < 1e-9:
            r = np.array([1.0, 0.0, 0.0]); nr = 1.0
    r = r / nr
    u = np.cross(f, r)
    # CIMA TEM DE SER CIMA.
    #
    # Ortonormal e com orientacao consistente nao garante isso, e a diferenca e
    # invisivel para qualquer teste que so compare a imagem com ela mesma: para
    # `costas34`, cross(f, r) da (0,0,-1) e a peca inteira renderiza de cabeca
    # para baixo. O usuario viu na primeira olhada; o instrument_check tinha
    # passado, porque testava consistencia da base e nunca a semantica dela.
    #
    # Inverter r JUNTO com u preserva cross(r, u) = f, entao a orientacao segue
    # a mesma em todas as cameras - so deixa de estar de ponta-cabeca.
    if float(np.dot(u, np.array([0.0, 0.0, 1.0]))) < 0.0:
        u = -u
        r = -r
    return r, u, f


def vertex_normals(verts, faces):
    n = np.zeros_like(verts)
    tri = verts[faces]
    fn = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    for k in range(3):
        np.add.at(n, faces[:, k], fn)
    ln = np.linalg.norm(n, axis=1, keepdims=True)
    ln[ln == 0] = 1.0
    return n / ln


class Frame:
    def __init__(self, w, h, bg=(1.0, 1.0, 1.0)):
        self.w, self.h = w, h
        self.rgb = np.ones((h, w, 3), np.float32) * np.array(bg, np.float32)
        self.z = np.full((h, w), np.inf, np.float32)

    def image(self):
        return np.clip(self.rgb, 0, 1)


def draw(frame: Frame, verts, faces, color, view, center, half, light_fill=0.25,
         cull=False, two_sided=False):
    """Rasterize one mesh into the frame. `color` is an (r,g,b) or per-vertex array."""
    right, up, fwd = view_basis(view)
    vn = vertex_normals(verts, faces)

    sx = (verts @ right - center[0]) / half
    sy = (verts @ up - center[1]) / half
    depth = verts @ fwd
    px = (sx * 0.5 + 0.5) * (frame.w - 1)
    py = (1.0 - (sy * 0.5 + 0.5)) * (frame.h - 1)

    # Headlight plus a soft fill from above: enough shape to read folds without
    # the hard terminator that hides detail on the shadowed side.
    lam = np.clip(-(vn @ fwd), 0, 1) * (1.0 - light_fill) + \
          np.clip(vn @ up, 0, 1) * light_fill
    shade = (0.28 + 0.72 * lam).astype(np.float32)

    base = np.asarray(color, np.float32)
    vcol = base[None, :] * shade[:, None] if base.ndim == 1 else base * shade[:, None]

    P = np.stack([px, py], axis=1)
    tri = faces
    a, b, c = P[tri[:, 0]], P[tri[:, 1]], P[tri[:, 2]]
    area = (b[:, 0] - a[:, 0]) * (c[:, 1] - a[:, 1]) - \
           (b[:, 1] - a[:, 1]) * (c[:, 0] - a[:, 0])
    keep = np.abs(area) > 1e-9
    if cull:
        # Facing decided by the GEOMETRIC NORMAL against the view direction, not
        # by screen-space winding.
        #
        # Winding was wrong in half the views and I showed him the results
        # anyway. Two of the seven view bases here are left-handed, which flips
        # the sign of the screen-space area, so `area > 0` kept the faces
        # pointing AWAY from the camera: the renderer was drawing the inside of
        # the mesh and hiding the surface. Self-tested on the closed body mesh,
        # where every visible face must be front-facing: screen-space area
        # scores 0.0% wrong in costas34 but 78.8% in perfil and 83.9% in frente,
        # while the normal test scores 0.0% in all three.
        #
        # dot(n, fwd) < 0 is basis-independent and needs no handedness rule, so
        # there is nothing left to get backwards.
        tri3 = verts[faces]
        gn = np.cross(tri3[:, 1] - tri3[:, 0], tri3[:, 2] - tri3[:, 0])
        keep &= (gn @ fwd) < 0.0
    idx = np.nonzero(keep)[0]

    xmin = np.floor(np.minimum(np.minimum(a[:, 0], b[:, 0]), c[:, 0])).astype(int)
    xmax = np.ceil(np.maximum(np.maximum(a[:, 0], b[:, 0]), c[:, 0])).astype(int)
    ymin = np.floor(np.minimum(np.minimum(a[:, 1], b[:, 1]), c[:, 1])).astype(int)
    ymax = np.ceil(np.maximum(np.maximum(a[:, 1], b[:, 1]), c[:, 1])).astype(int)
    np.clip(xmin, 0, frame.w - 1, out=xmin); np.clip(xmax, 0, frame.w - 1, out=xmax)
    np.clip(ymin, 0, frame.h - 1, out=ymin); np.clip(ymax, 0, frame.h - 1, out=ymax)

    dz = depth[tri]
    for t in idx:
        x0, x1, y0, y1 = xmin[t], xmax[t], ymin[t], ymax[t]
        if x1 < x0 or y1 < y0:
            continue
        xs = np.arange(x0, x1 + 1)
        ys = np.arange(y0, y1 + 1)
        gx, gy = np.meshgrid(xs, ys)
        ax, ay = a[t]; bx, by = b[t]; cx, cy = c[t]
        inv = 1.0 / area[t]
        w0 = ((bx - gx) * (cy - gy) - (by - gy) * (cx - gx)) * inv
        w1 = ((cx - gx) * (ay - gy) - (cy - gy) * (ax - gx)) * inv
        w2 = 1.0 - w0 - w1
        inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
        if not inside.any():
            continue
        zz = w0 * dz[t, 0] + w1 * dz[t, 1] + w2 * dz[t, 2]
        sub = frame.z[y0:y1 + 1, x0:x1 + 1]
        win = inside & (zz < sub)
        if not win.any():
            continue
        col = (w0[..., None] * vcol[tri[t, 0]] +
               w1[..., None] * vcol[tri[t, 1]] +
               w2[..., None] * vcol[tri[t, 2]])
        sub[win] = zz[win]
        frame.rgb[y0:y1 + 1, x0:x1 + 1][win] = col[win]


def frame_bounds(meshes, view, crop=None, pad=2.0):
    right, up, fwd = view_basis(view)
    allv = np.concatenate([m[0] for m in meshes])
    if crop:
        sel = (allv[:, 2] >= crop[0]) & (allv[:, 2] <= crop[1])
        if sel.any():
            allv = allv[sel]
    xs, ys = allv @ right, allv @ up
    cx = (xs.min() + xs.max()) * 0.5
    cy = (ys.min() + ys.max()) * 0.5
    half = max(xs.max() - xs.min(), ys.max() - ys.min()) * 0.5 + pad
    return (cx, cy), half
