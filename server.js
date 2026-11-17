const http = require('http');
const fs = require('fs');
const path = require('path');
const { execFile } = require('child_process');

const root = __dirname;
const port = Number(process.env.PORT) || 4173;
const allowedFiles = new Set([
    'index.html',
    'sim_status.json',
    'output_phase2.txt',
    'terminal_logs.txt'
]);
let runInProgress = false;

function sendJson(response, statusCode, data) {
    response.writeHead(statusCode, {
        'Content-Type': 'application/json; charset=utf-8',
        'Cache-Control': 'no-store'
    });
    response.end(JSON.stringify(data));
}

const server = http.createServer((request, response) => {
    const requestUrl = new URL(request.url, `http://${request.headers.host || 'localhost'}`);

    if (requestUrl.pathname === '/api/health' && request.method === 'GET') {
        return sendJson(response, 200, {ok: true});
    }

    if (requestUrl.pathname === '/api/run' && request.method === 'POST') {
        request.resume();
        if (runInProgress) {
            return sendJson(response, 409, {error: 'A simulator run is already in progress.'});
        }

        const executable = path.join(root, 'ph2.exe');
        if (!fs.existsSync(executable)) {
            return sendJson(response, 500, {error: 'ph2.exe is missing. Run run.bat to compile the simulator.'});
        }

        runInProgress = true;
        const startedAt = Date.now();
        return execFile(executable, [], {
            cwd: root,
            windowsHide: true,
            timeout: 30000,
            maxBuffer: 1024 * 1024
        }, (error, stdout, stderr) => {
            runInProgress = false;
            if (error) {
                return sendJson(response, 500, {
                    error: stderr.trim() || error.message,
                    stdout
                });
            }
            sendJson(response, 200, {ok: true, stdout, durationMs: Date.now() - startedAt});
        });
    }

    if (request.method !== 'GET' && request.method !== 'HEAD') {
        response.writeHead(405, {'Allow': 'GET, HEAD, POST'});
        return response.end('Method not allowed');
    }

    let fileName;
    try {
        fileName = decodeURIComponent(requestUrl.pathname.slice(1)) || 'index.html';
    } catch (error) {
        response.writeHead(400);
        return response.end('Invalid path');
    }
    if (!allowedFiles.has(fileName)) {
        response.writeHead(404);
        return response.end('Not found');
    }

    const filePath = path.join(root, fileName);
    fs.readFile(filePath, (error, contents) => {
        if (error) {
            response.writeHead(404);
            return response.end('Not found');
        }
        const contentType = fileName.endsWith('.html') ? 'text/html; charset=utf-8'
            : fileName.endsWith('.json') ? 'application/json; charset=utf-8'
            : 'text/plain; charset=utf-8';
        response.writeHead(200, {
            'Content-Type': contentType,
            'Cache-Control': 'no-store'
        });
        response.end(request.method === 'HEAD' ? undefined : contents);
    });
});

server.listen(port, '127.0.0.1', () => {
    const url = `http://localhost:${port}`;
    console.log(`MOS Simulator ready at ${url}`);
    if (process.argv.includes('--open') && process.platform === 'win32') {
        execFile('cmd.exe', ['/c', 'start', '', url], {windowsHide: true});
    }
});