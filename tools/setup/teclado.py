"""2.0.2: folha de icones de TECLADO no layout da do PS4: o nome da tecla entre parenteses, com a fonte do proprio jogo."""
import math
import os
import struct

# Fase 81: as teclas sao as do jogador (secao [keyboard] do launcher.ini). Quem chama (o launcher ou o jogo, depois do
# F3) manda a assinatura "acao=vk,...,mouse=0|1" (--keys), na ordem de ACTIONS; sem ela, as teclas padrao.
# Acao -> (recorte do PS4, tecla padrao). Mesma ordem e padroes de kKeyActions (src/hle/overrides/hle_pad.cpp e
# tools/dist/launcher.cpp).
ACTIONS = (
    ('cross', 'cross', 0x20), ('circle', 'circle', 0x1B), ('square', 'square', 0x58), ('triangle', 'triangle', 0x59),
    ('options', 'start', 0x0D), ('touchpad', 'select', 0x09),
    ('up', 'padup', 0x26), ('down', 'paddown', 0x28), ('left', 'padleft', 0x25), ('right', 'padright', 0x27),
    ('l1', 'l1', 0x51), ('r1', 'r1', 0x45), ('l2', 'l2', 0x31), ('r2', 'r2', 0x33), ('l3', 'l3', 0), ('r3', 'r3', 0),
    ('ls_up', 'lstickup', 0x57), ('ls_down', 'lstickdown', 0x53), ('ls_left', 'lstickleft', 0x41), ('ls_right', 'lstickright', 0x44),
    ('rs_up', 'rstickup', 0x49), ('rs_down', 'rstickdown', 0x4B), ('rs_left', 'rstickleft', 0x4A), ('rs_right', 'rstickright', 0x4C),
)

# Nomes curtos (a fonte do jogo e a das dicas do PC: letras, numeros e pontuacao comum)
VK_NAMES = {
    0x01: 'LMB', 0x02: 'RMB', 0x04: 'MMB', 0x05: 'Mouse4', 0x06: 'Mouse5',
    0x08: 'Backspace', 0x09: 'Tab', 0x0C: 'Clear', 0x0D: 'Enter', 0x10: 'Shift', 0x11: 'Ctrl', 0x12: 'Alt', 0x13: 'Pause',
    0x14: 'Caps', 0x1B: 'Esc', 0x20: 'Space', 0x21: 'PgUp', 0x22: 'PgDn', 0x23: 'End', 0x24: 'Home',
    0x25: 'Left', 0x26: 'Up', 0x27: 'Right', 0x28: 'Down', 0x2C: 'PrtSc', 0x2D: 'Ins', 0x2E: 'Del',
    0x5D: 'Menu', 0x6A: 'Num*', 0x6B: 'Num+', 0x6D: 'Num-', 0x6E: 'Num.', 0x6F: 'Num/', 0x90: 'NumLock', 0x91: 'ScrLk',
    0xA0: 'LShift', 0xA1: 'RShift', 0xA2: 'LCtrl', 0xA3: 'RCtrl', 0xA4: 'LAlt', 0xA5: 'RAlt',
    0xBA: ';', 0xBB: '=', 0xBC: ',', 0xBD: '-', 0xBE: '.', 0xBF: '/', 0xC0: '`', 0xDB: '[', 0xDC: '\\', 0xDD: ']', 0xDE: "'",
}


def key_name(vk):
    if 0x30 <= vk <= 0x39 or 0x41 <= vk <= 0x5A:
        return chr(vk)
    if 0x60 <= vk <= 0x69:
        return 'Num%d' % (vk - 0x60)
    if 0x70 <= vk <= 0x87:
        return 'F%d' % (vk - 0x6F)
    return VK_NAMES.get(vk, 'Key%02X' % vk)


def parse_keys(signature):
    """--keys "cross=32,...,mouse=1" -> ({acao: vk}, mouse). Acao que falta (ou sem --keys) fica com a tecla padrao."""
    vks = {name: vk for name, _sprite, vk in ACTIONS}
    mouse = False
    for item in (signature or '').split(','):
        name, _, value = item.partition('=')
        try:
            number = int(value)
        except ValueError:
            continue
        if name == 'mouse':
            mouse = number != 0
        elif name in vks and 0 <= number < 256:
            vks[name] = number
    return vks, mouse


def default_signature():
    return ','.join('%s=%d' % (name, vk) for name, _sprite, vk in ACTIONS) + ',mouse=0'


def stick_label(names):
    """Os 4 nomes (cima, esquerda, baixo, direita) do analogico: 'WASD' com letras; senao separados por /."""
    if any(not n for n in names):
        return ''
    if all(len(n) == 1 for n in names):
        return ''.join(names)
    if names == ['Up', 'Left', 'Down', 'Right']:
        return 'Arrows'
    return '/'.join(names)


def key_labels(signature=None):
    """Recorte do PS4 -> texto. Acao sem tecla fica sem texto (o recorte continua o do PS4)."""
    vks, mouse = parse_keys(signature)
    labels = {}
    for name, sprite, _default in ACTIONS:
        if vks[name]:
            labels[sprite] = key_name(vks[name])
    for stick, prefix in (('lstick', 'ls_'), ('rstick', 'rs_')):
        names = [key_name(vks[prefix + d]) if vks[prefix + d] else '' for d in ('up', 'left', 'down', 'right')]
        label = stick_label(names)
        if label:
            labels[stick] = label
    if mouse:
        labels['rstick'] = 'Mouse'  # o mouse e o analogico direito (camera)
    return labels



def parse_ft2(data, decode_dxt):
    """Fonte .ft2 da TT (big-endian): 'ROTV' com N x (x, y, largura) em float, 'ROTV' com (caractere, glifo) u16, DDS.
    decode_dxt: o decodificador de botoes.py (blocos, largura, altura, fourcc) -> linhas RGBA."""
    r1 = data.find(b'ROTV')
    r2 = data.find(b'ROTV', r1 + 4)
    line_h = struct.unpack_from('>f', data, r1 - 0x14)[0]
    n = struct.unpack_from('>I', data, r1 + 4)[0]
    glyphs = [struct.unpack_from('>3f', data, r1 + 8 + i * 12) for i in range(n)]
    m = struct.unpack_from('>I', data, r2 + 4)[0]
    chars = {}
    for i in range(m):
        c, g = struct.unpack_from('>HH', data, r2 + 8 + i * 4)
        if g < n:
            chars[chr(c)] = glyphs[g]
    dds = data.index(b'DDS ')
    height, width = struct.unpack_from('<2I', data, dds + 12)
    return {'chars': chars, 'line_h': line_h, 'width': width, 'height': height, 'fourcc': data[dds + 84:dds + 88],
            'blocks': data[dds + 128:], 'rows': {}, 'decode': decode_dxt}


def font_pixel_rows(font, y0, y1):
    """Linhas [y0, y1) do atlas, decodificadas so as faixas de blocos pedidas (o atlas inteiro demora)."""
    w, size = font['width'], (16 if font['fourcc'] == b'DXT5' else 8)
    bw = (w + 3) // 4
    for band in range(y0 // 4, (y1 + 3) // 4):
        if band not in font['rows']:
            chunk = font['blocks'][band * bw * size:(band + 1) * bw * size]
            font['rows'][band] = font['decode'](chunk, w, 4, font['fourcc'])
    return lambda y: font['rows'][y // 4][y % 4]


class Canvas:
    """RGBA em float, alfa nao pre-multiplicado; pinta por cima com cobertura (0..1)."""
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = [[0.0, 0.0, 0.0, 0.0] for _ in range(w * h)]

    def over(self, x, y, color, alpha):
        if alpha <= 0.0 or not (0 <= x < self.w and 0 <= y < self.h):
            return
        p = self.px[y * self.w + x]
        a = alpha + p[3] * (1.0 - alpha)
        if a <= 0.0:
            return
        for k in range(3):
            p[k] = (color[k] * alpha + p[k] * p[3] * (1.0 - alpha)) / a
        p[3] = a


def text_width(font, text, scale):
    gap = -font['line_h'] * 0.06
    total = 0.0
    for i, ch in enumerate(text):
        g = font['chars'].get(ch) or font['chars'].get(ch.upper())
        if g:
            total += g[2] + (gap if i else 0.0)
    return total * scale


def draw_text(canvas, font, text, cx, cy, height):
    """Texto centrado em (cx, cy) com altura de linha `height` (a fonte ja vem branca com contorno preto)."""
    line_h = font['line_h']
    scale = height / line_h
    gap = -line_h * 0.06
    x = cx - text_width(font, text, scale) / 2
    top = cy - height / 2
    for i, ch in enumerate(text):
        g = font['chars'].get(ch) or font['chars'].get(ch.upper())
        if not g:
            continue
        gx, gy, gw = g
        if i:
            x += gap * scale
        row = font_pixel_rows(font, int(gy), min(font['height'], int(gy + line_h) + 2))
        w, h = font['width'], font['height']
        for y in range(int(top), int(math.ceil(top + height))):
            fy = gy + (y + 0.5 - top) / scale
            if fy < gy or fy >= gy + line_h:
                continue
            for xx in range(int(x), int(math.ceil(x + gw * scale))):
                fx = gx + (xx + 0.5 - x) / scale
                if fx < gx or fx >= gx + gw:
                    continue
                # bilinear com alfa pre-multiplicado, so dentro do glifo
                fx0, fy0 = min(max(fx - 0.5, gx), gx + gw - 1), min(max(fy - 0.5, gy), gy + line_h - 1)
                ix, iy = int(fx0), int(fy0)
                ax, ay = fx0 - ix, fy0 - iy
                acc = [0.0, 0.0, 0.0, 0.0]
                for sx, sy, wgt in ((ix, iy, (1 - ax) * (1 - ay)), (ix + 1, iy, ax * (1 - ay)), (ix, iy + 1, (1 - ax) * ay),
                                    (ix + 1, iy + 1, ax * ay)):
                    sx, sy = min(sx, w - 1), min(sy, h - 1)
                    p = row(sy)[sx * 4:sx * 4 + 4]
                    a = p[3] / 255.0 * wgt
                    acc[0] += p[0] * a
                    acc[1] += p[1] * a
                    acc[2] += p[2] * a
                    acc[3] += a
                if acc[3] > 0.0:
                    canvas.over(xx, y, (acc[0] / acc[3], acc[1] / acc[3], acc[2] / acc[3]), min(1.0, acc[3]))
        x += gw * scale


def draw_sprite(name, label, w, h, frame, font):
    """Desenho de um recorte do PS4 (w x h pixels): o nome da tecla entre parenteses, como as dicas dos jogos LEGO de PC
    (ex. "(K)", "(Enter)"), com a fonte do jogo. A altura e a mesma em todos (metade do quadro original do icone, 128 px):
    os recortes tem tamanhos diferentes, mas o jogo desenha todos pelo quadro original; o que nao cabe encolhe."""
    c = Canvas(w, h)
    text = '(%s)' % label
    height = min(frame * 0.5, h * 0.95)
    tw = text_width(font, text, height / font['line_h'])
    if tw > w * 0.98:
        height *= w * 0.98 / tw
    draw_text(c, font, text, w / 2, h / 2, height)
    return c


def compose_keyboard(ps4, font, signature=None):
    """Folha no layout do PS4: cada recorte com tecla vira a tecla; o resto (sem tecla) fica o do PS4."""
    sprites4, w4, h4, _f, _b = ps4['sheet']
    image = [bytearray(row) for row in ps4['image']]
    labels = key_labels(signature)
    for name, (u0, v0, u1, v1, _ow, oh, *_rest) in sprites4.items():
        label = labels.get(name)
        if not label:
            continue
        x0, y0 = int(round(u0 * w4)), int(round((1.0 - v1) * h4))
        x1, y1 = int(round(u1 * w4)), int(round((1.0 - v0) * h4))
        c = draw_sprite(name, label, x1 - x0, y1 - y0, oh, font)
        for y in range(c.h):
            row = image[y0 + y]
            for x in range(c.w):
                p = c.px[y * c.w + x]
                row[(x0 + x) * 4:(x0 + x) * 4 + 4] = bytes((int(p[0] + 0.5), int(p[1] + 0.5), int(p[2] + 0.5), int(p[3] * 255 + 0.5)))
    return image


# Fase 81: o que a folha de teclas precisa (recortes do PS4 e a fonte do jogo) fica guardado ao lado das folhas: mudar
# as teclas (launcher ou F3 no jogo) refaz so a de teclado em segundos, sem ler os .DAT de novo.
CACHE_JSON = 'keyboard_src.json'
CACHE_FONT = 'keyboard_font.bin'


def save_cache(out_dir, ps4, font):
    import json
    sprites4, w4, h4, _f, _b = ps4['sheet']
    meta = {'sprites': {k: list(v) for k, v in sprites4.items()}, 'width': w4, 'height': h4,
            'font': {'chars': {k: list(v) for k, v in font['chars'].items()}, 'line_h': font['line_h'],
                     'width': font['width'], 'height': font['height'], 'fourcc': font['fourcc'].decode('latin-1')}}
    with open(os.path.join(out_dir, CACHE_FONT), 'wb') as f:
        f.write(font['blocks'])
    with open(os.path.join(out_dir, CACHE_JSON), 'w') as f:
        json.dump(meta, f)


def load_cache(out_dir, decode_dxt):
    """(ps4, fonte) do cache e de ps4.rgba, ou None se faltar algo."""
    import json
    try:
        with open(os.path.join(out_dir, CACHE_JSON)) as f:
            meta = json.load(f)
        with open(os.path.join(out_dir, CACHE_FONT), 'rb') as f:
            blocks = f.read()
        with open(os.path.join(out_dir, 'ps4.rgba'), 'rb') as f:
            rgba = f.read()
    except (OSError, ValueError):
        return None
    w4, h4 = meta['width'], meta['height']
    if len(rgba) != w4 * h4 * 4:
        return None
    sprites = {k: tuple(v) for k, v in meta['sprites'].items()}
    ps4 = {'sheet': (sprites, w4, h4, b'', b''), 'image': [rgba[y * w4 * 4:(y + 1) * w4 * 4] for y in range(h4)]}
    fm = meta['font']
    font = {'chars': {k: tuple(v) for k, v in fm['chars'].items()}, 'line_h': fm['line_h'], 'width': fm['width'],
            'height': fm['height'], 'fourcc': fm['fourcc'].encode('latin-1'), 'blocks': blocks, 'rows': {},
            'decode': decode_dxt}
    return ps4, font


def write_keyboard(out_dir, ps4, font, signature, png_writer=None):
    """keyboard.rgba + keyboard.keys (a assinatura das teclas com que foi feita; o launcher compara antes de abrir o jogo).
    Escreve num temporario e troca: o jogo pode estar lendo a folha."""
    signature = signature or default_signature()
    image = compose_keyboard(ps4, font, signature)
    temp = os.path.join(out_dir, 'keyboard.rgba.tmp')
    with open(temp, 'wb') as f:
        f.write(b''.join(bytes(row) for row in image))
    os.replace(temp, os.path.join(out_dir, 'keyboard.rgba'))
    with open(os.path.join(out_dir, 'keyboard.keys'), 'w') as f:
        f.write(signature + '\n')
    if png_writer:
        _s, w4, h4, _f, _b = ps4['sheet']
        png_writer(os.path.join(out_dir, 'keyboard.png'), image, w4, h4)
