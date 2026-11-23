// What the engine's built-in decoders can play. The server enforces the same
// list (audio_extensions() in server/src/net/control_server.cpp), and Electron's
// file dialog in client/electron/main.js carries its own copy.
export const PLAYABLE_AUDIO_EXTENSIONS = ['wav', 'aiff', 'aif', 'flac', 'mp3'] as const;

const PLAYABLE = new RegExp(`\\.(${PLAYABLE_AUDIO_EXTENSIONS.join('|')})$`, 'i');

export const isPlayableAudio = (name: string): boolean => PLAYABLE.test(name);
