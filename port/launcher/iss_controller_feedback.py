"""Launcher-only integration. Creates private derived audio; never edits the dump."""
from pathlib import Path
import hashlib, json, os, sys, tempfile

SOURCE_HASH='687730fd454719e4671b6757914ac4439b744447f9838d4d6d8547d9c9856352'
BANK_HASH='271867fd226b5ec51f194048c6c27760634bf260cf656f3cbfa3be3ac8f898c5'

def prepare(settings, user, extra=None):
    settings=dict(settings)
    env=dict(extra or {})
    env.setdefault('ISS_CONTROLLER_LIGHTBAR',os.environ.get('ISS_CONTROLLER_LIGHTBAR','1'))
    env.setdefault('ISS_PAD_SPEAKER_DEVICE',os.environ.get('ISS_PAD_SPEAKER_DEVICE','auto'))
    env.setdefault('ISS_PAD_SPEAKER_GAIN',os.environ.get('ISS_PAD_SPEAKER_GAIN','4.0'))
    if os.environ.get('ISS_CONCRETE_SPEAKER_CUE','1')!='1':return settings,env
    try:
        import concrete_audio_overlay, concrete_refill_bank, iss_controller_links as mods
        base=Path(getattr(sys,'_MEIPASS',Path(__file__).resolve().parent))/'controller-feedback'
        concrete_audio_overlay.ROOT=base
        game=Path(settings['game_dir']).resolve(strict=True)
        source=game/'art/cache/common_sound.sprig.xpps'
        if hashlib.sha256(source.read_bytes()).hexdigest()!=SOURCE_HASH:
            print('Controller speaker: unverified audio bank; added Concrete cue disabled, original audio retained.',flush=True)
            return settings,env
        cache=Path(user)/'controller-feedback'
        target=cache/'banks'/BANK_HASH/'common_sound.sprig.xpps'
        if not target.exists() or hashlib.sha256(target.read_bytes()).hexdigest()!=BANK_HASH:
            target.parent.mkdir(parents=True,exist_ok=True)
            pending=target.with_suffix('.pending')
            manifest=concrete_audio_overlay.create_overlay(source,pending,concrete_refill_bank.patch_standalone_bank)
            assert manifest['overlay_sha256']==BANK_HASH
            os.replace(pending,target)
        overlays=cache/'overlays';overlays.mkdir(exist_ok=True)
        overlay=Path(tempfile.mkdtemp(prefix='concrete-',dir=overlays))
        for entry in game.iterdir():mods.make_link(overlay/entry.name,entry)
        mods.expand(overlay/'art');mods.expand(overlay/'art/cache')
        dest=overlay/'art/cache/common_sound.sprig.xpps'
        assert mods.is_link(dest)
        mods.remove_link(dest);mods.make_link(dest,target)
        assert hashlib.sha256(dest.read_bytes()).hexdigest()==BANK_HASH
        settings['game_dir']=str(overlay)
        env.update(ISS_CONTROLLER_EVENT_TRACE='1',ISS_CONTROLLER_REFILL_TRACE='1',ISS_CONCRETE_SPEAKER_CUE='1')
        print('Controller speaker: private Concrete cue enabled; original game assets unchanged.',flush=True)
    except (OSError,ValueError,AssertionError,KeyError,ImportError) as exc:
        print('Controller speaker: added Concrete cue unavailable; using original assets: '+str(exc),flush=True)
    return settings,env

def install(module):
    original=module['launch']
    def launch(settings,seconds=0,extra_env=None):
        if os.name!='nt' or not module['renderer_v2'](settings):return original(settings,seconds,extra_env)
        settings,env=prepare(settings,module['user_dir'](),extra_env)
        return original(settings,seconds,env)
    module['launch']=launch
