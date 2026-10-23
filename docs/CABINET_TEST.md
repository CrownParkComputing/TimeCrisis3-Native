# Cabinet test and maintenance

![Nuklear cabinet maintenance controls](screenshots/tc3-cabinet-test-menu.png)

Press **F10**, then select **Cabinet / Test** with Tab or the mouse. Use arrows and Enter for menu navigation. The shared frontend offers these actions to both TC3 and TC4:

| Nuklear action | Original cabinet input |
| --- | --- |
| Toggle original test menu | JVS test DIP switch 0, same board control as F9 |
| Test: Up / Down / Left / Right | Original JVS direction switches |
| Test: Enter / Button 1 | Original JVS Button 1 |
| Service credit | Original JVS service switch |
| Insert coin | Original coin input |

Selecting an action closes Nuklear and resumes the original game so it can process that control. Press F10 again to issue another maintenance action. Direction, Enter and Service actions use a short pulse measured in guest video fields, then release; they do not remain held when the menu reopens. The test switch stays toggled until toggled again or the original board configuration changes.

The original game screen remains responsible for gun calibration, sound tests, I/O tests, coin settings and link settings. Read its prompts and use its exit/save choices normally. These controls do not write game settings directly or create replacement service menus. Keyboard F9, Up/Down, E (Button 1), 9 (Service), 5 (Coin) remain available when Nuklear is closed; left/right keyboard availability follows your input configuration.

For TC3 linked workers, a test/service action targets the cabinet whose menu you opened and resumes the pair. Use the right window's menu to maintain the right cabinet. Changing a link or gun setting on one cabinet may require repeating the original procedure on the other. TC4 currently supports a single diagnostic cabinet; TC3's transport is not used for it.

Hardware calibration and full maintenance workflows need testing on the intended gun/cabinet setup. Use private copies of SRAM and dongles as described in the setup guide. No factory reset or storage-erasing action is added to Nuklear.
