export type Configuration = Record<string, unknown> & {
  fallback_mode?: string;
  gamepad?: string;
};

export interface PreparationCommand {
  do: string;
  undo: string;
  elevated?: boolean | string;
}

export interface ServerCommand {
  name: string;
  cmd: string;
  elevated?: boolean | string;
}

interface DisplayStatus {
  name: string;
  deviceIndex: number;
  drmCardIndex: number;
  width: number;
  height: number;
  fps: number;
  frameUpdates?: number;
  zeroCopyCapture?: boolean;
  capturePath?: string;
  outputIndex?: number;
  clientName?: string;
}

export interface DisplayBackendStatus {
  diagnostic?: string;
  runningKernel?: string;
  moduleLoaded?: boolean;
  deviceCount?: number;
  dkmsKernels?: string[];
  activeDisplays?: DisplayStatus[];
}

export interface EvdiStatus extends DisplayBackendStatus {
  exclusiveLayoutSupported?: boolean;
  outputLayoutBackend?: string;
  libraryInstalled?: boolean;
  libraryVersion?: string;
  sessionType?: string;
}

export interface HermesKmsStatus extends DisplayBackendStatus {
  devicePresent?: boolean;
  driverVersion?: string;
  moduleInstalled?: boolean;
  installedVersion?: string;
  uapiVersion?: number;
  requiredUapiVersion?: number;
  newestKnownUapiVersion?: number;
  outputCount?: number;
  multiOutputCapable?: boolean;
  multiDeviceCapable?: boolean;
  privateSeatBrokerCount?: number;
  missingPrivateSeatBrokers?: string[];
  inaccessibleNode?: string;
  reloadNote?: string;
  newerNote?: string;
}

export interface ClipboardStatus {
  diagnostic?: string;
  available?: boolean;
  sessionType?: string;
  waylandReadAvailable?: boolean;
  waylandWriteAvailable?: boolean;
  x11Available?: boolean;
  missingTools?: string[];
  manualInstall?: string;
}

export interface ConfigurationResponse extends Configuration {
  platform: string;
  version?: string;
  version_commit?: string;
  evdiSetupRequired?: boolean;
  evdiDiagnostic?: string;
  evdiInfo?: EvdiStatus;
  hermesKmsDiagnostic?: string;
  hermesKmsInfo?: HermesKmsStatus;
  clipboardInfo?: ClipboardStatus;
}
