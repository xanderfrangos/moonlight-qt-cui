Moonlight deep trace launcher (Windows)

1. Extract Moonlight Deep Trace.bat beside Moonlight.exe in your portable folder.
   Use a local disk, not a network share or a mapped network drive.
2. Close Moonlight completely, then double-click Moonlight Deep Trace.bat.
3. Start your usual stream with VRR enabled and reproduce the problem.
4. Close Moonlight normally and wait for the launcher to finish.
5. Open VRR-Logs in the portable folder. Zip the entire newly created session
   folder and send it back, including all connection files. Include approximately
   when you noticed the problem and what it looked like.

Each launch gets a unique folder. It contains Moonlight.log, the deep .vrrtrace
capture and any archived connection segments, executable identity, and exit code.
Moonlight.stdout.log may be empty. If no VRR trace is created, send Moonlight.log.

The launcher enables tracing only. It preserves your settings and inherited
device-sharing overrides. It does not enable Windows GPU ETW tracing, upload
files, change portable mode, or install anything. No administrator rights needed.
Launch Moonlight normally when you no longer want tracing.
