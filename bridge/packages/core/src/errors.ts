import { statusName, type StatusName } from "./constants.ts";

/** Malformed frame / stream desynchronization. The connection must be closed. */
export class NdpProtocolError extends Error {
  readonly status: number;
  constructor(status: number, message: string) {
    super(`${statusName(status)}: ${message}`);
    this.name = "NdpProtocolError";
    this.status = status;
  }
}

/** The agent answered a request with an ERR frame. */
export class NdpRemoteError extends Error {
  readonly status: number;
  readonly statusName: StatusName | `UNKNOWN_${number}`;
  readonly detail: string | undefined;
  readonly osResult: number | undefined;
  constructor(status: number, detail?: string, osResult?: number) {
    super(`agent error ${statusName(status)}${detail ? `: ${detail}` : ""}`);
    this.name = "NdpRemoteError";
    this.status = status;
    this.statusName = statusName(status);
    this.detail = detail;
    this.osResult = osResult;
  }
}

/** Local problem: timeout, connection lost, bad response shape. */
export class NdpTransportError extends Error {
  /** Node/system error code (ECONNREFUSED, EHOSTUNREACH, ...) when the failure came from the socket. */
  readonly code: string | undefined;
  constructor(message: string, code?: string) {
    super(message);
    this.name = "NdpTransportError";
    this.code = code;
  }
}
