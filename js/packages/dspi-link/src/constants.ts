/*
 * The protocol's fixed names and numbers, in one place.
 *
 * The client, discovery and the web app all need the port, the WebSocket path
 * and the version this library speaks. Each value is defined here once so they
 * cannot drift apart. Implements Link spec 3.2 (port and path), 4 (the
 * sub-protocol) and 7.2 (the protocol version sent in hello).
 */

/** WebSocket sub-protocol every hub requires (Link spec 4). */
export const LINK_SUBPROTOCOL = "dspi-link-1";

/** TCP port hubs listen on by default (Link spec 3). */
export const LINK_DEFAULT_PORT = 11915;

/** Path of the WebSocket endpoint; the major version is part of it (Link spec 10, rule 1). */
export const LINK_WS_PATH = "/dspi/v1";

/** Path of the HTTP discovery endpoint (Link spec 3.2). */
export const LINK_INFO_PATH = `${LINK_WS_PATH}/info`;

/** The protocol version this library implements, sent in the client hello (Link spec 7.2). */
export const LINK_PROTO_VERSION = { major: 1, minor: 2 } as const;

/**
 * This library's own version, sent as the client's `version` in hello when
 * the application gives none. Kept equal to package.json by a test.
 */
export const LINK_LIBRARY_VERSION = "1.2.0";
