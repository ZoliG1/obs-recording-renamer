# Stream Deck plugin

"Add word", "Remove last word", "Save" and "Delete recording" buttons for the Recording Renamer window. Each button has its own word;
pressing it sends the word to OBS (obs-websocket vendor request `recording-renamer` / `add_word`).

Build: `npm install && npx esbuild src/plugin.js --bundle --platform=node --target=node20 --outfile=sdPlugin/bin/plugin.js --external:bufferutil --external:utf-8-validate`,
then zip the `sdPlugin` folder renamed to `com.zolig1.recordingrenamer.sdPlugin` as `RecordingRenamer.streamDeckPlugin`.
