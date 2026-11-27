# runtime/input

| File | What it is |
|---|---|
| `input.c`, `input.h`, `input_map.*` | Controller state and remapping; one gamepad per player for local multiplayer (`ORBITALS_VIRTUAL_PADS=N` adds N virtual SDL gamepads to try it without controllers); a remote keyboard as player 2 and the Steam virtual gamepads of Remote Play Together guests ([steam/](../steam/README.md)); the last device used (keyboard or controller, for the button icons) |
| `input_script.*` | Button script (`ORBITALS_INPUT_SCRIPT`; a `p<N> ` prefix presses for player N, also through `ctl press`) and the input recording (`ORBITALS_INPUT_RECORD`); `ORBITALS_CTL` presses |
| `input_sdl.*`, `input_wsys.h` | SDL3 back end |
| `motion_fusion.c` | Gyro and accelerometer |
| `switch2_proto.h` | The Switch 2 controllers' Bluetooth LE protocol (Pro Controller 2, Joy-Con 2: GATT ids, handshake, reports, stick calibration, rumble, pairing), from seedds/mkw-neo (MIT) with our button bits; checked by `tests/test_switch2.py` |
| `switch2.h`, `switch2.c` | Switch 2 controllers over Bluetooth LE (`ORBITALS_SWITCH2`, off by default; SDL has them over USB only): the session on top of a transport (identity, pairing, calibration, features, player lights, rumble), each pad a virtual SDL gamepad with its gyro, so players, layout and rumble work as for any gamepad |
| `linux_switch2.c` | Its Linux transport: BlueZ over the system D-Bus, libdbus-1 loaded at run time; `ORBITALS_SWITCH2=2` logs each advertising device once |
| `switch2_win.c` | Its Windows transport: WinRT through its C ABI (advertisement watcher, `BluetoothLEDevice`, GATT), its events queued for the transport's thread; `ORBITALS_SWITCH2=2` as on Linux |

Default keyboard (`input_map.c`, changeable in the `.ini`):

| Key | Button | In MKW |
|---|---|---|
| WASD | left stick | steering, menus |
| Mouse, arrows | right stick | camera; world map zoom |
| Space, Enter, E | A | accelerate, accept |
| Left Shift, Backspace | B | brake, back |
| F, X | ZR | drift |
| R | R | drift |
| Q | L | item |
| Z, right click | ZL | item |
| Left click | Y | |
| V | X | |
| 1-4 | D-pad | |
| Tab | + | pause |
| M | − | |
