# -*- coding: utf-8 -*-
"""
Live Arena: create /Game/Maps/Arena containing one ArenaVenue actor (UE 5.4 - 5.6 editor, PythonScriptPlugin).

Why: the game already runs on the empty /Engine/Maps/Entry map (AArenaGameMode spawns a venue itself), but a saved
level with a placed ArenaVenue lets you swap SeatMesh / GuestChairMesh / ... in the Details panel and keep them.

Run it in the editor (the C++ module must be compiled and loaded):
  - Tools > Execute Python Script... > pick this file, or
  - Output Log, switch the input box to "Python", then:   import create_arena_level; create_arena_level.main()
    (Content/Python is on the editor's Python path), or
  - Output Log "Cmd":   py "<project>/Content/Python/create_arena_level.py"

Safe to run again: if the map already exists it is opened, and a venue is only added when there is none.
"""

import traceback

import unreal

MAP_PATH = "/Game/Maps/Arena"
VENUE_CLASS_PATH = "/Script/LiveArena.ArenaVenue"
VENUE_LABEL = "ArenaVenue"
AUDIENCE_CLASS_PATH = "/Script/LiveArena.ArenaAudience"
AUDIENCE_LABEL = "ArenaAudience"

_TAG = "[LiveArena]"


class ArenaSetupError(Exception):
    """A step failed in a way the user can fix; the message says how."""


def _log(msg):
    unreal.log("{} {}".format(_TAG, msg))


def _warn(msg):
    unreal.log_warning("{} {}".format(_TAG, msg))


def _error(msg):
    unreal.log_error("{} {}".format(_TAG, msg))


def _load_venue_class():
    venue_class = None
    try:
        venue_class = unreal.load_class(None, VENUE_CLASS_PATH)
    except Exception as exc:  # load_class raises on some versions, returns None on others
        _warn("load_class({}) failed: {}".format(VENUE_CLASS_PATH, exc))
    if venue_class is None:
        venue_class = getattr(unreal, "ArenaVenue", None)
    if venue_class is None:
        raise ArenaSetupError(
            "Cannot find class {}. Build the LiveArena C++ module (open LiveArena.uproject and let it compile, "
            "or build LiveArenaEditor in your IDE), restart the editor, then run this script again.".format(VENUE_CLASS_PATH))
    return venue_class


def _ensure_audience(actor_subsystem):
    """Places one ArenaAudience so FigureMesh / MaxLabels can be set in Details (the game mode reuses it)."""
    try:
        audience_class = unreal.load_class(None, AUDIENCE_CLASS_PATH)
    except Exception:
        audience_class = getattr(unreal, "ArenaAudience", None)
    if audience_class is None:
        _warn("Cannot find {}; skipping the audience actor (the game spawns one at runtime).".format(AUDIENCE_CLASS_PATH))
        return
    for actor in actor_subsystem.get_all_level_actors():
        if actor is not None and actor.get_class().get_name() == "ArenaAudience":
            _log("Level already has an ArenaAudience; not adding another.")
            return
    audience = actor_subsystem.spawn_actor_from_class(audience_class, unreal.Vector(0.0, 0.0, 0.0), unreal.Rotator(0.0, 0.0, 0.0))
    if audience is None:
        _warn("spawn_actor_from_class({}) returned None.".format(AUDIENCE_CLASS_PATH))
        return
    try:
        audience.set_actor_label(AUDIENCE_LABEL)
    except Exception:
        pass
    _log("Spawned ArenaAudience at the origin.")


def _subsystems():
    level_subsystem = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    if level_subsystem is None or actor_subsystem is None:
        raise ArenaSetupError("LevelEditorSubsystem / EditorActorSubsystem unavailable (is this the full editor?).")
    return level_subsystem, actor_subsystem


def _ensure_not_playing(level_subsystem):
    try:
        playing = level_subsystem.is_in_play_in_editor()
    except AttributeError:
        playing = False
    if playing:
        raise ArenaSetupError("Stop Play-In-Editor first (Esc), then run the script again.")


def _save_dirty_work():
    """new_level / load_level discard unsaved changes in the open level, so offer to save them first."""
    try:
        ok = unreal.EditorLoadingAndSavingUtils.save_dirty_packages_with_dialog(True, True)
    except Exception as exc:
        _warn("Could not show the save dialog ({}); continuing.".format(exc))
        return
    if not ok:
        raise ArenaSetupError("Saving was cancelled; nothing changed.")


def _open_or_create_map(level_subsystem):
    if unreal.EditorAssetLibrary.does_asset_exist(MAP_PATH):
        _log("{} already exists, opening it.".format(MAP_PATH))
        if not level_subsystem.load_level(MAP_PATH):
            raise ArenaSetupError("Could not open {}.".format(MAP_PATH))
        return False

    _log("Creating {} ...".format(MAP_PATH))
    if not level_subsystem.new_level(MAP_PATH):
        raise ArenaSetupError(
            "new_level('{}') failed. Check the Output Log; make sure the path is writable and not checked out "
            "by someone else.".format(MAP_PATH))
    return True


def _is_venue(actor, venue_class):
    if actor is None:
        return False
    try:
        return unreal.MathLibrary.class_is_child_of(actor.get_class(), venue_class)
    except Exception:
        return actor.get_class().get_name() == "ArenaVenue"


def _find_venues(actor_subsystem, venue_class):
    return [a for a in actor_subsystem.get_all_level_actors() if _is_venue(a, venue_class)]


def _spawn_venue(actor_subsystem, venue_class):
    # Actor origin = centre of the stage's front edge; the audience bowl extends toward -X.
    venue = actor_subsystem.spawn_actor_from_class(venue_class, unreal.Vector(0.0, 0.0, 0.0), unreal.Rotator(0.0, 0.0, 0.0))
    if venue is None:
        raise ArenaSetupError("spawn_actor_from_class({}) returned None.".format(VENUE_CLASS_PATH))
    try:
        venue.set_actor_label(VENUE_LABEL)
    except Exception as exc:
        _warn("Could not rename the venue actor: {}".format(exc))
    return venue


def _select(actor_subsystem, actor):
    try:
        actor_subsystem.set_selected_level_actors([actor])
    except Exception:
        pass  # selection is only a convenience


def _print_next_steps(created):
    lines = [
        "",
        "==================== Live Arena: {} {} ====================".format(MAP_PATH, "created" if created else "updated"),
        "下一步 / Next steps:",
        "  1. Edit > Project Settings > Project > Maps & Modes:",
        "       Editor Startup Map  -> {}".format(MAP_PATH),
        "       Game Default Map    -> {}".format(MAP_PATH),
        "     (Default GameMode stays ArenaGameMode.)",
        "  2. 喺 Outliner 揀 ArenaVenue，Details > Meshes 可以換：",
        "       SeatMesh (+ SeatMeshOffset), GuestChairMesh, FollowSpotMesh, BlockoutMaterial (needs a 'Color' vector param).",
        "     Details > Layout: NumRows, FirstRowRadius, RowDepth, RowRise, SeatSpacing, ArcDegrees, Stage*/Screen* sizes.",
        "     Changing the layout changes seat counts; the game sends the new layout to the server on connect.",
        "     揀 ArenaAudience，Details > Audience > FigureMesh 換觀眾公仔 (pivot at the seat floor point, facing +X, seated).",
        "  3. Edit > Project Settings > Game > Live Arena: ServerUrl, RoomId, HostKey, JoinUrl, YouTubeVideoId, StreamerName ...",
        "  4. Save (Ctrl+S) after editing, then Play (Alt+P) or launch Standalone and capture the window in OBS.",
        "  Lights, fog, camera, audience and the LED are spawned at runtime, so the level looks dark in the editor: that is expected.",
        "=" * 92,
    ]
    for line in lines:
        unreal.log(line)


def main():
    try:
        venue_class = _load_venue_class()
        level_subsystem, actor_subsystem = _subsystems()
        _ensure_not_playing(level_subsystem)
        _save_dirty_work()

        created = _open_or_create_map(level_subsystem)

        venues = _find_venues(actor_subsystem, venue_class)
        if venues:
            _log("Level already has {} ArenaVenue actor(s); not adding another.".format(len(venues)))
            if len(venues) > 1:
                _warn("More than one ArenaVenue: the game mode uses the first one it finds. Delete the extras.")
            venue = venues[0]
        else:
            venue = _spawn_venue(actor_subsystem, venue_class)
            _log("Spawned ArenaVenue at the origin.")

        _ensure_audience(actor_subsystem)
        _select(actor_subsystem, venue)

        if not level_subsystem.save_current_level():
            raise ArenaSetupError("save_current_level() failed; save manually with Ctrl+S.")
        _log("Saved {}.".format(MAP_PATH))

        _print_next_steps(created)
        return True
    except ArenaSetupError as exc:
        _error(str(exc))
    except Exception:
        _error("Unexpected error:\n" + traceback.format_exc())
    return False


if __name__ == "__main__":
    main()
