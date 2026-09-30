# Gamepads

DUDE inherits dhewm3's SDL game-controller support: every controller SDL recognises is
opened, its axes and buttons arrive as `JOY_*` keys (see `base/gamepad.cfg`), and the
ImGui settings menu can be navigated with the pad. This page covers the DUDE additions.

## Which pad feeds input: `in_gamepadDevice`

Some pads show up twice, for instance a controller plugged in through its USB cable
while its wireless dongle is also connected: SDL then lists an idle "Xbox 360
Controller" (the dongle) next to the live pad. In-game input merges every pad, so play
works either way, but the ImGui backend used to bind its navigation to the *first*
controller only, which could be the idle dongle. The menu then ignored the pad you held.

- `in_gamepadDevice ""` (default): input from every connected pad is merged, in-game
  and in the ImGui menus.
- `in_gamepadDevice <GUID or name>`: only that pad feeds input; the others are ignored.
  The auto-detected button-name layout (`joy_gamepadLayout -1`) follows the chosen pad.
- `listGamepads` prints the connected pads with GUID and device path, marking with `*`
  the ones currently feeding input.
- Settings menu → Control Options → Gamepad Settings → **Gamepad Device** is the same
  choice as a dropdown. Two pads with the same name are told apart by their device path.

A selected pad that is not connected falls back to "all" until it reappears. Hotplug is
handled: pads are registered on `SDL_JOYDEVICEADDED` and dropped on removal.

## Default Xbox-style bindings

A fresh install has no gamepad bindings; the defaults live in `base/gamepad.cfg` (Doom 3)
and `base/gamepad-d3xp.cfg` (Resurrection of Evil, Lost Mission and mods based on d3xp,
whose weapon slots differ). Settings menu → Bindings → **Reset Gamepad Bindings to
Xbox-style Defaults** applies the matching file after a confirmation: it clears all
`JOY_*` bindings and rebinds

| Control | Action |
|---|---|
| Left stick / click | Move / Run |
| Right stick / click | Look / Crouch |
| A | Jump |
| X | Reload |
| Back | PDA |
| LB / RB | Previous / Next weapon |
| D-Pad | Weapon shortcuts |
| LT / RT | Flashlight / Attack |

Keyboard and mouse bindings are left untouched. The same result from the console is
`exec gamepad.cfg` (or `exec gamepad-d3xp.cfg`), which additionally keeps any old
gamepad bindings the file does not override.

## Implementation notes

- `neo/sys/events.cpp`: `gamepads` registry (`AddGamepad` / `RemoveGamepad`),
  `UpdateActiveGamepad()` resolves the cvar, controller events from non-selected pads are
  dropped in `Sys_GetEvent()`, `Sys_Get*Gamepad*()` expose the list to the menu.
- `neo/sys/sys_imgui.cpp`: `SetGamepads()` puts the SDL2/SDL3 ImGui backend into its
  *Manual* gamepad mode with exactly the selected handles (merged by the backend: buttons
  OR-ed, axes max). Manual mode also stops the backend from closing handles it does not
  own. The selection is cached so it survives ImGui being initialised after the input
  layer (Vulkan path) and `vid_restart`.
- `neo/framework/Dhewm3SettingsMenu.cpp`: the dropdown and the reset button, which
  parses the cfg with `idLexer` so the bindings table refreshes immediately.
