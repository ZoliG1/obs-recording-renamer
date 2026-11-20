# Recording Renamer for OBS Studio

After you stop a recording, a small window asks for a name and puts it in front of the file name:

```
 - PS4 - 19.11.2026 19-48-55.mp4  ->  Johnny hack - PS4 - 19.11.2026 19-48-55.mp4
```

- Delete = move the clip to the Recycle Bin (asks first).
- Enter = save, Esc / Skip / timeout = keep the original name. Typing pauses the countdown.
- **Scripts** dock with a checkbox to turn it on or off.
- **Tools → Recording Renamer Settings**: on/off, timeout (or no timeout), test on last recording.
- Uses OBS's own Qt interface, so it follows the OBS theme.

Built for Windows x64, OBS 31 or newer. Builds run on GitHub Actions on every push.
