/**
 * Input device enumeration and constraints (plan §L 6A).
 *
 * Permission is NEVER requested on page load (M6): every function here is
 * called from an explicit user action.
 */

export interface InputDeviceInfo {
  deviceId: string;
  label: string;
  /** iOS Safari exposes no usable deviceId — the UI must say so honestly. */
  selectable: boolean;
}

export interface AppliedSettings {
  sampleRate: number | null;
  channelCount: number | null;
  echoCancellation: boolean | null;
  noiseSuppression: boolean | null;
  autoGainControl: boolean | null;
  latencyS: number | null;
  deviceId: string | null;
  /** Constraints we asked for that the browser did not honour. */
  ignored: string[];
}

/** Raw-input constraints: processing OFF so takes are the actual performance. */
export function rawConstraints(deviceId?: string): MediaStreamConstraints {
  return {
    audio: {
      ...(deviceId ? { deviceId: { exact: deviceId } } : {}),
      echoCancellation: false,
      noiseSuppression: false,
      autoGainControl: false,
      channelCount: 1,
    },
    video: false,
  };
}

export async function listInputs(): Promise<InputDeviceInfo[]> {
  try {
    const devices = await navigator.mediaDevices.enumerateDevices();
    return devices
      .filter((d) => d.kind === "audioinput")
      .map((d) => ({
        deviceId: d.deviceId,
        label: d.label || "input (label hidden until permission is granted)",
        selectable: d.deviceId !== "" && d.deviceId !== "default",
      }));
  } catch {
    return [];
  }
}

/** What the browser ACTUALLY applied, plus what it silently ignored. */
export function readSettings(stream: MediaStream): AppliedSettings {
  const track = stream.getAudioTracks()[0];
  if (!track) return { sampleRate: null, channelCount: null, echoCancellation: null, noiseSuppression: null, autoGainControl: null, latencyS: null, deviceId: null, ignored: ["no audio track"] };
  const s = track.getSettings() as MediaTrackSettings & { latency?: number };
  const ignored: string[] = [];
  if (s.echoCancellation) ignored.push("echoCancellation");
  if (s.noiseSuppression) ignored.push("noiseSuppression");
  if (s.autoGainControl) ignored.push("autoGainControl");
  return {
    sampleRate: s.sampleRate ?? null,
    channelCount: s.channelCount ?? null,
    echoCancellation: typeof s.echoCancellation === "boolean" ? s.echoCancellation : null,
    noiseSuppression: typeof s.noiseSuppression === "boolean" ? s.noiseSuppression : null,
    autoGainControl: typeof s.autoGainControl === "boolean" ? s.autoGainControl : null,
    latencyS: s.latency ?? null,
    deviceId: s.deviceId ?? null,
    ignored,
  };
}
