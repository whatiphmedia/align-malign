# Auto Malign control surface for Ableton Live (Live 11 / 12).
#
# Tells every Auto Malign plugin how much latency sits in front of it on its track (synths, effects, racks), so
# Auto Malign can go last in the chain and still learn every track at its true song position.
#
# Install: the plugin's Pro page > Alignment > "Install for Ableton Live" copies this folder to
#   <Documents>/Ableton/User Library/Remote Scripts/AutoMalign
# then in Live: Settings > Link, Tempo & MIDI > Control Surface > "AutoMalign" (no input/output ports needed).
#
# How it finds the plugins: each Auto Malign instance publishes a random ID through its "Link ID" parameter.
# Twice a second this script walks every track's devices (including inside racks), adds up the latency of
# everything before each Auto Malign, and sends {id, latency, track} to the plugins over UDP on 127.0.0.1.
from __future__ import absolute_import
import json
import socket

try:
    from _Framework.ControlSurface import ControlSurface
except ImportError:  # very old Live
    ControlSurface = object

PORT = 47863
LINK_PARAM = "Link ID"


def create_instance(c_instance):
    return AutoMalign(c_instance)


class AutoMalign(ControlSurface):
    def __init__(self, c_instance):
        ControlSurface.__init__(self, c_instance)
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._tick = 0
        self._last = None
        self.show_message("Auto Malign: linked")

    def disconnect(self):
        try:
            self._sock.close()
        except Exception:
            pass
        ControlSurface.disconnect(self)

    # Live calls this roughly every 100 ms
    def update_display(self):
        ControlSurface.update_display(self)
        self._tick += 1
        if self._tick % 5:
            return
        try:
            found = self._scan()
            payload = json.dumps({"am": 1, "sr": self._sample_rate(), "tracks": found})
            # resend every second even if unchanged, so newly opened plugins hear it quickly
            if payload != self._last or self._tick % 10 == 0:
                self._last = payload
                self._sock.sendto(payload.encode("utf-8"), ("127.0.0.1", PORT))
        except Exception as e:  # never break Live
            self.log_message("Auto Malign: %s" % e)

    def _sample_rate(self):
        try:
            return int(self.application().get_document().sample_rate)  # not on every Live version
        except Exception:
            return 0

    def _scan(self):
        song = self.song()
        out = []
        tracks = list(song.tracks) + list(song.return_tracks) + [song.master_track]
        for t in tracks:
            self._walk(t.devices, 0, t.name, out)
        return out

    def _walk(self, devices, base, track_name, out):
        acc = base
        for d in devices:
            link = self._link_id(d)
            if link:
                out.append({"id": link, "latency": int(acc), "track": track_name})
            if getattr(d, "can_have_chains", False):
                for ch in getattr(d, "chains", []):
                    self._walk(ch.devices, acc, track_name, out)
            acc += self._latency(d)

    @staticmethod
    def _latency(d):
        try:
            return int(getattr(d, "latency_in_samples", 0) or 0)
        except Exception:
            return 0

    @staticmethod
    def _link_id(d):
        try:
            for p in d.parameters:
                if p.name == LINK_PARAM:
                    lo, hi = float(p.min), float(p.max)
                    v = (float(p.value) - lo) / (hi - lo) if hi > lo else float(p.value)
                    lid = int(round(v * 100000.0))
                    return lid if lid > 0 else None
        except Exception:
            pass
        return None
