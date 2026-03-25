#!/usr/bin/env node

/**
 * Olive AI Studio MCP Bridge
 *
 * Bridges stdio-based MCP clients (like Claude Code CLI) to the HTTP-based
 * MCP server running inside Unreal Editor.
 *
 * Features:
 *   - Auto-discovers the Olive AI MCP server on ports 3000-3009
 *   - Translates stdio JSON-RPC to HTTP JSON-RPC
 *   - Handles connection errors gracefully
 *
 * Usage:
 *   node mcp-bridge.js [--port 3000] [--host localhost]
 */

const http = require('http');
const readline = require('readline');

// Configuration
const DEFAULT_HOST = 'localhost';
const PORT_RANGE_START = 3000;
const PORT_RANGE_END = 3009;
const DISCOVERY_TIMEOUT = 500; // ms per port check

// Hosts to try during auto-discovery (in order of priority)
// localhost: bridge runs on the same machine as UE
// host.docker.internal: bridge runs inside Docker, UE on Windows/Mac host
// 172.17.0.1: Docker bridge gateway fallback (Linux host)
const DISCOVERY_HOSTS = ['localhost', 'host.docker.internal', '172.17.0.1'];

// Parse command line arguments
const args = process.argv.slice(2);
let serverPort = null; // null means auto-discover
let serverHost = DEFAULT_HOST;

for (let i = 0; i < args.length; i++) {
    if (args[i] === '--port' && i + 1 < args.length) {
        serverPort = parseInt(args[i + 1], 10);
        i++;
    } else if (args[i] === '--host' && i + 1 < args.length) {
        serverHost = args[i + 1];
        i++;
    }
}

// Log to stderr (stdout is reserved for MCP protocol responses)
function log(message) {
    console.error(`[Olive MCP Bridge] ${message}`);
}

// Check if MCP server is running on a specific port
function checkPort(host, port) {
    return new Promise((resolve) => {
        const postData = JSON.stringify({
            jsonrpc: '2.0',
            id: 'discovery',
            method: 'ping',
            params: {}
        });

        const req = http.request({
            hostname: host,
            port: port,
            path: '/mcp',
            method: 'POST',
            timeout: DISCOVERY_TIMEOUT,
            headers: {
                'Content-Type': 'application/json',
                'Content-Length': Buffer.byteLength(postData)
            }
        }, (res) => {
            let data = '';
            res.on('data', chunk => data += chunk);
            res.on('end', () => {
                try {
                    const response = JSON.parse(data);
                    // Check if this looks like our MCP server
                    if (response.jsonrpc === '2.0') {
                        resolve(true);
                    } else {
                        resolve(false);
                    }
                } catch {
                    resolve(false);
                }
            });
        });

        req.on('error', () => resolve(false));
        req.on('timeout', () => {
            req.destroy();
            resolve(false);
        });

        req.write(postData);
        req.end();
    });
}

// Auto-discover MCP server port across one or more hosts.
// When a specific host is provided (via --host), only that host is tried.
// Otherwise, tries each host in DISCOVERY_HOSTS until a server is found.
// Returns { host, port } or null if no server is found.
async function discoverPort(hosts) {
    for (const host of hosts) {
        log(`Discovering Olive AI MCP server on ${host}...`);

        for (let port = PORT_RANGE_START; port <= PORT_RANGE_END; port++) {
            const found = await checkPort(host, port);
            if (found) {
                log(`Found MCP server at ${host}:${port}`);
                return { host, port };
            }
        }
    }

    return null;
}

// Send HTTP request to MCP server
function forwardRequest(host, port, jsonRpcRequest) {
    return new Promise((resolve, reject) => {
        const postData = JSON.stringify(jsonRpcRequest);

        const req = http.request({
            hostname: host,
            port: port,
            path: '/mcp',
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
                'Content-Length': Buffer.byteLength(postData)
            }
        }, (res) => {
            let data = '';
            res.on('data', chunk => data += chunk);
            res.on('end', () => {
                try {
                    resolve(JSON.parse(data));
                } catch (err) {
                    reject(new Error(`Failed to parse response: ${err.message}`));
                }
            });
        });

        req.on('error', (err) => {
            reject(new Error(`Connection failed: ${err.message}. Is Unreal Editor running with Olive AI Studio?`));
        });

        req.write(postData);
        req.end();
    });
}

// Create JSON-RPC error response
function createErrorResponse(id, code, message) {
    return {
        jsonrpc: '2.0',
        id: id,
        error: { code, message }
    };
}

// Main async entry point
async function main() {
    // Determine which host and port to use
    let activeHost = serverHost;
    let activePort = serverPort;

    // Track whether the user explicitly set --host
    const userSpecifiedHost = args.includes('--host');

    if (activePort !== null && !userSpecifiedHost) {
        // Port is known (e.g., from .mcp.json --port), but host is not.
        // Try candidate hosts on the known port (handles Docker environments).
        for (const host of DISCOVERY_HOSTS) {
            const found = await checkPort(host, activePort);
            if (found) {
                activeHost = host;
                log(`Found MCP server at ${host}:${activePort}`);
                break;
            }
        }
    }

    if (activePort === null) {
        // Full auto-discover across candidate hosts and port range
        const hostsToTry = userSpecifiedHost ? [serverHost] : DISCOVERY_HOSTS;
        const result = await discoverPort(hostsToTry);

        if (result !== null) {
            activeHost = result.host;
            activePort = result.port;
        } else {
            const triedHosts = hostsToTry.join(', ');
            log(`ERROR: Could not find Olive AI MCP server on ports ${PORT_RANGE_START}-${PORT_RANGE_END}`);
            log(`Tried hosts: ${triedHosts}`);
            log('Make sure Unreal Editor is running with the Olive AI Studio plugin enabled.');
            log('If running inside Docker, ensure the UE editor is reachable via host.docker.internal.');
            log('Check Window > Developer Tools > Output Log for: "MCP Server started on port XXXX"');

            // Send a helpful error for the first request and exit
            const rl = readline.createInterface({
                input: process.stdin,
                output: process.stdout,
                terminal: false
            });

            rl.once('line', (line) => {
                try {
                    const request = JSON.parse(line);
                    console.log(JSON.stringify(createErrorResponse(
                        request.id,
                        -32000,
                        'Olive AI MCP server not found. Ensure Unreal Editor is running with Olive AI Studio enabled. If running in Docker, verify the host is reachable via host.docker.internal.'
                    )));
                } catch {
                    console.log(JSON.stringify(createErrorResponse(
                        null,
                        -32000,
                        'Olive AI MCP server not found. Ensure Unreal Editor is running with Olive AI Studio enabled. If running in Docker, verify the host is reachable via host.docker.internal.'
                    )));
                }
                process.exit(1);
            });

            return;
        }
    }

    log(`Connected to Olive AI MCP server at ${activeHost}:${activePort}`);

    // Setup readline for stdin
    const rl = readline.createInterface({
        input: process.stdin,
        output: process.stdout,
        terminal: false
    });

    // Handle incoming JSON-RPC requests (concurrent — don't await between lines)
    rl.on('line', (line) => {
        if (!line.trim()) return;

        let request;
        try {
            request = JSON.parse(line);
        } catch (err) {
            log(`Parse error: ${err.message}`);
            console.log(JSON.stringify(createErrorResponse(null, -32700, 'Parse error')));
            return;
        }

        const method = request.method || 'unknown';
        log(`→ ${method}`);

        forwardRequest(activeHost, activePort, request)
            .then(response => {
                log(`← ${method}: ${response.error ? 'error' : 'ok'}`);
                console.log(JSON.stringify(response));
            })
            .catch(err => {
                log(`Error: ${err.message}`);
                console.log(JSON.stringify(createErrorResponse(
                    request.id,
                    -32603,
                    err.message
                )));
            });
    });

    rl.on('close', () => {
        log('Connection closed');
        process.exit(0);
    });

    // Handle termination signals
    process.on('SIGINT', () => process.exit(0));
    process.on('SIGTERM', () => process.exit(0));
}

// Run
main().catch(err => {
    log(`Fatal error: ${err.message}`);
    process.exit(1);
});
