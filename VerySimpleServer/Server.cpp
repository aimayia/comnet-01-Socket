#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <stdio.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <map>
#include <cstring>
#include <algorithm>

using namespace std;

#pragma comment(lib,"ws2_32.lib")

static string getIniString(const char* file, const char* section, const char* key, const char* def) {
    char buf[512] = {0};
    GetPrivateProfileStringA(section, key, def, buf, sizeof(buf), file);
    return string(buf);
}

static string contentTypeByExt(const string& path) {
    size_t p = path.find_last_of('.');
    if (p==string::npos) return "application/octet-stream";
    string ext = path.substr(p+1);
    if(ext=="htm"||ext=="html") return "text/html; charset=utf-8";
    if(ext=="css") return "text/css";
    if(ext=="js") return "application/javascript";
    if(ext=="png") return "image/png";
    if(ext=="jpg"||ext=="jpeg") return "image/jpeg";
    if(ext=="gif") return "image/gif";
    if(ext=="txt") return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

void main(){
    // 1. 读取配置（server.ini 与可执行文件同目录）
    char exePath[MAX_PATH]; GetModuleFileNameA(NULL, exePath, MAX_PATH);
    string exeDir = exePath;
    size_t pos = exeDir.find_last_of("\\/");
    if(pos!=string::npos) exeDir = exeDir.substr(0, pos);
    string iniFile = exeDir + "\\server.ini";

    string listenAddr = getIniString(iniFile.c_str(), "server", "address", "0.0.0.0");
    string portStr     = getIniString(iniFile.c_str(), "server", "port", "5050");
    string webroot     = getIniString(iniFile.c_str(), "server", "root", exeDir.c_str());

    cout << "Config: address=" << listenAddr << " port=" << portStr << " root=" << webroot << endl;

    // 如果配置的 webroot 中没有 index.html，则尝试回退到 exe 的父目录（常见于在 Debug 目录运行 exe）
    auto fileExists = [](const string& p)->bool {
        DWORD attr = GetFileAttributesA(p.c_str());
        return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
    };

    string testIndex = webroot;
    if(testIndex.size() && testIndex.back()!='\\' && testIndex.back()!='/') testIndex += "\\";
    testIndex += "index.html";
    if (!fileExists(testIndex)) {
        // 取 exeDir 的父目录
        string parent = exeDir;
        size_t ppos = parent.find_last_of("\\/");
        if (ppos != string::npos) parent = parent.substr(0, ppos);
        string parentIndex = parent;
        if(parentIndex.size() && parentIndex.back()!='\\' && parentIndex.back()!='/') parentIndex += "\\";
        parentIndex += "index.html";
        if (fileExists(parentIndex)) {
            cout << "webroot: index.html not found in configured root, switching webroot to parent: " << parent << endl;
            webroot = parent;
        } else {
            cout << "Warning: index.html not found in configured webroot (" << webroot << ") or its parent (" << parent << ")" << endl;
        }
    }

    // 2. Winsock 初始化与 socket/create/bind/listen
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0) {
        cout << "WSAStartup failed\n"; return;
    }

    SOCKET srvSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srvSocket == INVALID_SOCKET) { cout << "socket() failed\n"; WSACleanup(); return; }

    // allow reuse
    BOOL opt = TRUE;
    setsockopt(srvSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)atoi(portStr.c_str()));
    if(listenAddr == "0.0.0.0" || listenAddr.empty()) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        // Parse IPv4 address: on Windows some SDKs may not declare InetPtonA, so fall back to inet_addr.
        #ifdef _WIN32
        addr.sin_addr.s_addr = inet_addr(listenAddr.c_str());
        #else
        inet_pton(AF_INET, listenAddr.c_str(), &addr.sin_addr);
        #endif
    }

    if (bind(srvSocket, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        cout << "bind() failed\n"; closesocket(srvSocket); WSACleanup(); return;
    }

    if (listen(srvSocket, SOMAXCONN) == SOCKET_ERROR) {
        cout << "listen() failed\n"; closesocket(srvSocket); WSACleanup(); return;
    }

    cout << "Server listening on " << listenAddr << ":" << portStr << endl;

    // 3. accept 循环并处理简单 HTTP GET
    while (true) {
        sockaddr_in clientAddr; int clientLen = sizeof(clientAddr);
        SOCKET client = accept(srvSocket, (sockaddr*)&clientAddr, &clientLen);
        if (client == INVALID_SOCKET) {
            cout << "accept() failed\n"; break;
        }

                // 获取客户端 IP（用于日志）
                char clientIpBuf[INET_ADDRSTRLEN] = {0};
        #ifdef _WIN32
                const char* tmpIp = inet_ntoa(clientAddr.sin_addr);
                if (tmpIp) strcpy_s(clientIpBuf, sizeof(clientIpBuf), tmpIp);
        #else
                inet_ntop(AF_INET, &clientAddr.sin_addr, clientIpBuf, INET_ADDRSTRLEN);
        #endif
                string clientIp = clientIpBuf;

        // 接收请求（简单实现：一次 recv 读取全部请求行/头部）
        char buf[8192] = {0};
        int ret = recv(client, buf, sizeof(buf)-1, 0);
        if (ret <= 0) { closesocket(client); continue; }

        // 解析请求行： METHOD PATH HTTP/...
        istringstream reqstream(buf);
        string method, path, httpver;
        reqstream >> method >> path >> httpver;

        // 日志：客户端与请求行
        cout << "[" << clientIp << "] " << method << " " << path << endl;

        // 健康检查端点，便于验证服务是否启动与配置
        if (method == "GET" && (path == "/__health" || path == "/__health/")) {
            ostringstream oss;
            oss << "HTTP/1.1 200 OK\r\n";
            oss << "Content-Type: application/json; charset=utf-8\r\n";
            string body = "{\"status\":\"ok\",\"address\":\"" + listenAddr + "\",\"port\":\"" + portStr + "\",\"root\":\"" + webroot + "\"}";
            oss << "Content-Length: " << body.size() << "\r\n";
            oss << "Connection: close\r\n\r\n";
            oss << body;
            string resp = oss.str();
            send(client, resp.c_str(), (int)resp.size(), 0);
            closesocket(client);
            continue;
        }

        // 仅处理 GET
        if (method != "GET") {
            string resp = "HTTP/1.1 405 Method Not Allowed\r\nConnection: close\r\n\r\nMethod Not Allowed";
            send(client, resp.c_str(), (int)resp.size(), 0);
            closesocket(client);
            continue;
        }

        // 规范化路径
        if (path == "/") path = "/index.html";
        // 防止目录穿越
        while(path.find("..") != string::npos) path.erase(path.find(".."), 2);

        string fullPath = webroot;
        if(fullPath.back()!='\\' && fullPath.back()!='/') fullPath += "\\";
        // 把 / 转为 \
        for(char &c : path) if(c=='/') c='\\';
        fullPath += (path.size() && (path[0]=='\\' || path[0]=='/') ? path.substr(1) : path);

        // 4. 从文件系统读取并构建响应
        ifstream ifs(fullPath, ios::binary);
        if (!ifs) {
            string body = "<html><body><h1>404 Not Found</h1></body></html>";
            ostringstream oss;
            oss << "HTTP/1.1 404 Not Found\r\n";
            oss << "Content-Type: text/html; charset=utf-8\r\n";
            oss << "Content-Length: " << body.size() << "\r\n";
            oss << "Connection: close\r\n\r\n";
            oss << body;
            string resp = oss.str();
            send(client, resp.c_str(), (int)resp.size(), 0);
            closesocket(client);
            continue;
        }

        // 读取文件内容
        ifs.seekg(0, ios::end);
        size_t fsize = ifs.tellg();
        ifs.seekg(0, ios::beg);
        string content;
        content.resize(fsize);
        ifs.read(&content[0], fsize);

        // 日志：返回文件和大小
        cout << "[" << clientIp << "] Serving file: " << fullPath << " (" << fsize << " bytes)" << endl;

        // 构造响应头并发送（头 + 二进制内容）
        string ctype = contentTypeByExt(fullPath);
        ostringstream header;
        header << "HTTP/1.1 200 OK\r\n";
        header << "Content-Type: " << ctype << "\r\n";
        header << "Content-Length: " << fsize << "\r\n";
        header << "Connection: close\r\n\r\n";
        string hdr = header.str();
        send(client, hdr.c_str(), (int)hdr.size(), 0);

        // send body (可能较大时需要循环发送，这里简单一次性发送)
        size_t sent = 0;
        while (sent < content.size()) {
            int s = send(client, content.data() + sent, (int)min<size_t>(8192, content.size()-sent), 0);
            if (s == SOCKET_ERROR) break;
            sent += s;
        }

        closesocket(client);
    }

    closesocket(srvSocket);
    WSACleanup();
}