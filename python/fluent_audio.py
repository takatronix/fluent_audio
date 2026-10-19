"""fluent_audio from Python — a thin ctypes wrapper over the C API (libfluent_audio.so). No build step.
(The design's nanobind module is M4; this covers tools, tests and server-side use now.)

    import fluent_audio as fa
    s = fa.Sound.load("presets/looks.yaml", sample_rate=48000)
    s.select("kitt"); s.set("amount", 0.8)
    left, right = s.render(mono_samples)           # list / array('f') / numpy → same type back (numpy if given)
    fa.nodes()                                     # every node type and its param table (from the C++ registry)

The library is looked up in $FLUENT_AUDIO_LIB, then ../build/libfluent_audio.so next to this file.
"""
import array
import ctypes
import json
import os

_HERE = os.path.dirname(os.path.abspath(__file__))
_lib = ctypes.CDLL(os.environ.get("FLUENT_AUDIO_LIB") or os.path.join(_HERE, "..", "build", "libfluent_audio.so"))
_P = ctypes.c_void_p
for name, res, args in [
    ("fa_create_ex", _P, [ctypes.c_char_p, ctypes.c_float, ctypes.c_int, ctypes.c_char_p]),
    ("fa_error", ctypes.c_char_p, []), ("fa_destroy", None, [_P]), ("fa_digest", ctypes.c_char_p, [_P]),
    ("fa_in_buffer", ctypes.POINTER(ctypes.c_float), [_P, ctypes.c_int]),
    ("fa_out_buffer", ctypes.POINTER(ctypes.c_float), [_P, ctypes.c_int]),
    ("fa_process", ctypes.c_int, [_P, ctypes.c_int]),
    ("fa_set_param_f32", ctypes.c_int, [_P, ctypes.c_char_p, ctypes.c_float]),
    ("fa_get_param_f32", ctypes.c_float, [_P, ctypes.c_char_p]),
    ("fa_param_count", ctypes.c_int, [_P]), ("fa_param_name", ctypes.c_char_p, [_P, ctypes.c_int]),
    ("fa_select", ctypes.c_int, [_P, ctypes.c_char_p]), ("fa_preset_count", ctypes.c_int, [_P]),
    ("fa_preset_name", ctypes.c_char_p, [_P, ctypes.c_int]), ("fa_bypass", ctypes.c_int, [_P, ctypes.c_int]),
    ("fa_reset", None, [_P]), ("fa_diagnostics", ctypes.c_char_p, [_P]), ("fa_nodes_json", ctypes.c_char_p, []),
]:
    f = getattr(_lib, name)
    f.restype, f.argtypes = res, args


def nodes():
    """Every registered node type with its parameter table."""
    return json.loads(_lib.fa_nodes_json().decode())


class Sound:
    def __init__(self, yaml_text, sample_rate=48000, block=512, search_dir=None):
        self._fa = _lib.fa_create_ex(yaml_text.encode(), sample_rate, block, search_dir.encode() if search_dir else None)
        if not self._fa:
            raise ValueError(_lib.fa_error().decode())
        self.sample_rate, self.block = sample_rate, block

    @classmethod
    def load(cls, path, sample_rate=48000, block=512):
        with open(path, encoding="utf-8") as f:
            return cls(f.read(), sample_rate, block, os.path.dirname(os.path.abspath(path)))

    def __del__(self):
        if getattr(self, "_fa", None):
            _lib.fa_destroy(self._fa)

    @property
    def presets(self):
        return [_lib.fa_preset_name(self._fa, i).decode() for i in range(_lib.fa_preset_count(self._fa))]

    @property
    def params(self):
        return [_lib.fa_param_name(self._fa, i).decode() for i in range(_lib.fa_param_count(self._fa))]

    @property
    def digest(self):
        return _lib.fa_digest(self._fa).decode()

    def select(self, look):
        if not _lib.fa_select(self._fa, look.encode()):
            raise KeyError(look)

    def set(self, name, value):
        if not _lib.fa_set_param_f32(self._fa, name.encode(), float(value)):
            raise KeyError(f"{name}: {_lib.fa_diagnostics(self._fa).decode()}")

    def get(self, name):
        return _lib.fa_get_param_f32(self._fa, name.encode())

    def bypass(self, on=True):
        _lib.fa_bypass(self._fa, 1 if on else 0)

    def reset(self):
        _lib.fa_reset(self._fa)

    def render(self, mono, tail_seconds=0.0):
        """Process mono samples (continuing the state); returns (left, right)."""
        np = None
        if type(mono).__module__ == "numpy":
            import numpy as np
            x = np.ascontiguousarray(mono, dtype=np.float32)
        else:
            x = array.array("f", mono)
        n = len(x) + int(tail_seconds * self.sample_rate)
        left, right = array.array("f", bytes(4 * n)), array.array("f", bytes(4 * n))
        inb, o0, o1 = (_lib.fa_in_buffer(self._fa, 0), _lib.fa_out_buffer(self._fa, 0), _lib.fa_out_buffer(self._fa, 1))
        for pos in range(0, n, self.block):
            m = min(self.block, n - pos)
            for i in range(m):
                inb[i] = float(x[pos + i]) if pos + i < len(x) else 0.0
            _lib.fa_process(self._fa, m)
            ctypes.memmove(ctypes.addressof(ctypes.c_float.from_buffer(left, pos * 4)), o0, m * 4)
            ctypes.memmove(ctypes.addressof(ctypes.c_float.from_buffer(right, pos * 4)), o1, m * 4)
        if np is not None:
            return np.frombuffer(left, np.float32).copy(), np.frombuffer(right, np.float32).copy()
        return left, right
