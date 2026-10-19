# Optional gyro aiming

The port can add a controller's **real gyroscope** to the right stick **only while L2 is held**. It is disabled by default. The physical right stick, keyboard and mouse still work. Releasing L2 immediately removes the gyro contribution. This needs a controller/mode that actually exposes a gyro to SDL (for example DualShock 4, DualSense or Switch Pro); an XInput-only device cannot provide missing sensor data.

Enable **Launcher > Controls > Gyro while holding L2** before launching, or **Insert > Controls > Gyro aiming > Gyro while holding L2** during play. Launcher changes are read on the next launch; Insert changes apply immediately and are saved when the menu closes.

| `tlg.ini` key | Environment override | Default | Meaning |
|---|---|---|---|
| `gyro_aim` | `ISS_GYRO_AIM` / `TLG_GYRO_AIM` | `0` | Enable the L2-held gyro contribution |
| `gyro_sensitivity` | `ISS_GYRO_SENS` / `TLG_GYRO_SENS` | `2.5` | 0.1–5; recommended starting range 2–2.5; 180 degrees/s maps to a full stick at 1x |
| `gyro_invert_y` | `ISS_GYRO_INVERT_Y` / `TLG_GYRO_INVERT_Y` | `0` | Reverse vertical gyro aiming |
| `gyro_smoothing` | `ISS_GYRO_SMOOTHING` / `TLG_GYRO_SMOOTHING` | `0.025` | Exponential filter time constant, 0–0.2 seconds; 0 disables it |
| `gyro_noise` | `ISS_GYRO_NOISE` / `TLG_GYRO_NOISE` | `0.01` | Radial noise threshold, 0–0.1 radians/s |
| `gyro_deadzone` | `ISS_GYRO_DEADZONE` / `TLG_GYRO_DEADZONE` | `0.1` | Anti-dead zone to overcome the game's stick dead zone, 0–0.5 |

Start with sensitivity **2–2.5** (default **2.5**), then adjust to preference and the controller. Existing saved sensitivity values are preserved. This is a starting recommendation, not a hardware-independent calibration. If the aim drifts with the controller still, increase the noise threshold slightly; if slow movement does nothing, try a slightly larger anti-dead zone. Smoothing reduces jitter but adds latency. This uses SDL's calibrated gyro readings, with an adjustable noise threshold; it does not add a new bias-calibration procedure.

SDL pitch (X) controls vertical aim and yaw (Y) controls horizontal aim; roll (Z) is ignored. Angular velocity maps to stick deflection, so the game's existing stick response, acceleration, sensitivity and maximum turn speed still apply. It is not a direct camera-angle patch. Adding the gyro and stick can saturate at the stick limits.

The gyro contribution stops when the game window loses focus, the port menu captures input, the controller disconnects, sensor reading fails, or the touchpad/graffiti simulation owns input. Long sampling gaps reset the filter. Real orientation, acceleration and angular velocity sent to the original game are unchanged; the synthetic graffiti sensor is never used as an aiming source. `ISS_MOTION=off` / `TLG_MOTION=off` also disables real gyro aiming.

## Tests

`port/tests/test_gyro_aim.c` checks axis signs, Y inversion, sensitivity, noise, rate-independent smoothing, release, clamping, stick addition and invalid inputs. `test_gyro_pad.c` uses SDL's virtual gyro and accelerometer to exercise the real pad path: opt-in/L2 gate, focus/menu, untouched motion data, graffiti/touchpad suppression, disconnect and gyro-only hardware. Neither needs a game dump or physical controller.

On Linux these are part of `bash build.sh --test`; on Windows build the CMake targets `gyro-aim-test` and `gyro-pad-test`. The existing Qt Controls tests cover the launcher toggle, defaults and saving the new controls without changing graffiti settings. Hardware/in-game comfort still needs player testing.

## Português

Ative **Launcher > Controls > Giroscópio ao segurar L2** antes de iniciar, ou **Insert > Controls > Mira por giroscópio** durante o jogo. A opção vem desligada e só soma o movimento real do controle à mira enquanto L2 está pressionado; o analógico direito continua funcionando. O controle precisa expor o giroscópio ao SDL (por exemplo no modo Switch Pro). O modo XInput sem sensor não fornece esses dados.

As mudanças do launcher valem na próxima partida; as do menu Insert são imediatas. Comece com sensibilidade **2–2,5** (padrão **2,5**) e ajuste ao controle e à sua preferência; os valores já salvos são preservados. É uma recomendação inicial, não uma calibração universal. Aumente um pouco o limiar de ruído se a mira deslizar em repouso, ou a anti-zona morta se movimentos lentos não fizerem nada. A suavização reduz tremor e aumenta o atraso. A resposta e o limite de velocidade do analógico do jogo continuam valendo.

Soltar L2, perder o foco, abrir o menu do port ou entrar na simulação do grafite/touchpad remove a contribuição do giroscópio. Os dados reais de movimento usados pelo grafite não são alterados. Os testes automáticos não substituem a avaliação da mira durante uma partida.
