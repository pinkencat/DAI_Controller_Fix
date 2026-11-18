xinput9_1_0.dll  -  controller slot fix for Dragon Age: Inquisition
=====================================================================

DA:I only reads input from XInput slot 0 ("Player 1"). Windows assigns
slots first-come, first-served and never re-packs them. If anything is
holding slot 0 when the controller connects, the controller lands in slot 1+.
That could be an earlier controller, a pad that briefly re-enumerates while
switching modes (the GameSir G7 SE shows up as an Xbox 360 pad for a moment
before it switches to Xbox One mode), or a virtual pad from Razer Synapse,
AMD, Steam, DS4Windows and so on. The game sees that a controller exists and
shows gamepad prompts, but it never reads that slot, so every press is lost.

What this DLL does
------------------
The game loads this DLL instead of C:\Windows\System32\xinput9_1_0.dll.
It forwards every call to the real Windows DLL but renumbers the slots so
that connected controllers always appear as slots 0, 1, 2, ... in order.
The first connected controller is therefore always Player 1, whatever slot
Windows gave it. It re-checks once a second, so hot-plugging still works.

Log / settings
--------------
xinput_remap.log is written next to the DLL each time the game starts. It
shows which physical slot your controller is in, which slots the game polls,
and the first few button presses the game received.

Optional xinput_remap.ini next to the DLL:
    [xinput_remap]
    Remap=1     ; 0 = don't renumber (pass-through, for testing)
    Log=1       ; 0 = no log file

Uninstall
---------
Delete xinput9_1_0.dll (and xinput_remap.log / .ini) from the game folder.
Nothing else is changed. The game exe and data files are untouched.
