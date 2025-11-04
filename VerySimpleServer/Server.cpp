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
#include <cctype>

using namespace std;

#pragma comment(lib,"ws2_32.lib")

static string getIniString(const char* file, const char* section, const char* key, const char* def) {
    char buf[512] = {0};
    GetPrivateProfileStringA(section, key, def, buf, sizeof(buf), file);
    return string(buf);
}

// 修改：扩展名小写化以支持不同大小写的文件后缀
static string contentTypeByExt(const string& path) {
    size_t p = path.find_last_of('.');
    if (p==string::npos) return "application/octet-stream";
    string ext = path.substr(p+1);
    // normalize to lower-case
    transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return (char)tolower(c); });
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

    // 新增：统一发送简单响应的 helper
    auto sendSimpleResponse = [&](SOCKET client, int code, const string& status, const string& body, const string& ctype="text/html; charset=utf-8"){
        ostringstream oss;
        oss << "HTTP/1.1 " << code << " " << status << "\r\n";
        oss << "Content-Type: " << ctype << "\r\n";
        oss << "Content-Length: " << body.size() << "\r\n";
        oss << "Connection: close\r\n\r\n";
        oss << body;
        string resp = oss.str();
        send(client, resp.c_str(), (int)resp.size(), 0);
    };

    // 3. accept 循环并处理简单 HTTP GET
    while (true) {
        sockaddr_in clientAddr; int clientLen = sizeof(clientAddr);
        SOCKET client = accept(srvSocket, (sockaddr*)&clientAddr, &clientLen);
        if (client == INVALID_SOCKET) {
            cout << "accept() failed\n"; break;
        }

        // 获取客户端 IP 和端口（用于日志）
        char clientIpBuf[INET_ADDRSTRLEN] = {0};
#ifdef _WIN32
        const char* tmpIp = inet_ntoa(clientAddr.sin_addr);
        if (tmpIp) strcpy_s(clientIpBuf, sizeof(clientIpBuf), tmpIp);
#else
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIpBuf, INET_ADDRSTRLEN);
#endif
        string clientIp = clientIpBuf;
        int clientPort = ntohs(clientAddr.sin_port);

        // 接收请求（一次 recv）
        char buf[8192] = {0};
        int ret = recv(client, buf, sizeof(buf)-1, 0);
        if (ret <= 0) { closesocket(client); continue; }

        // 提取第一行请求行并严格解析
        string reqAll = string(buf, ret);
        size_t eol = reqAll.find("\r\n");
        string reqLine = (eol==string::npos) ? reqAll : reqAll.substr(0, eol);
        istringstream lineStream(reqLine);
        string method, path, httpver;
        if (!(lineStream >> method >> path >> httpver)) {
            // 400 Bad Request
            cout << "[" << clientIp << ":" << clientPort << "] " << reqLine << " -> 400 Bad Request" << endl;
            string body = "<html><body><h1>400 Bad Request</h1></body></html>";
            sendSimpleResponse(client, 400, "Bad Request", body);
            closesocket(client);
            continue;
        }

        // 日志：客户端与请求行
        cout << "[" << clientIp << ":" << clientPort << "] " << method << " " << path << " " << httpver << endl;

        // 健康检查端点
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
            cout << "[" << clientIp << ":" << clientPort << "] " << reqLine << " -> 200 OK" << endl;
            closesocket(client);
            continue;
        }

        // 仅处理 GET
        if (method != "GET") {
            cout << "[" << clientIp << ":" << clientPort << "] " << reqLine << " -> 405 Method Not Allowed" << endl;
            string resp = "HTTP/1.1 405 Method Not Allowed\r\nConnection: close\r\n\r\nMethod Not Allowed";
            send(client, resp.c_str(), (int)resp.size(), 0);
            closesocket(client);
            continue;
        }

        // 规范化路径与防穿越
        if (path == "/") path = "/index.html";
        while(path.find("..") != string::npos) path.erase(path.find(".."), 2);

        string fullPath = webroot;
        if(fullPath.back()!='\\' && fullPath.back()!='/') fullPath += "\\";
        for(char &c : path) if(c=='/') c='\\';
        fullPath += (path.size() && (path[0]=='\\' || path[0]=='/') ? path.substr(1) : path);

        // 文件存在性与权限检查
        DWORD attr = GetFileAttributesA(fullPath.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) {
            cout << "[" << clientIp << ":" << clientPort << "] " << reqLine << " -> 404 Not Found (" << fullPath << ")" << endl;
            string body = "<html><body><h1>404 Not Found</h1></body></html>";
            sendSimpleResponse(client, 404, "Not Found", body);
            closesocket(client);
            continue;
        }
        if (attr & FILE_ATTRIBUTE_DIRECTORY) {
            cout << "[" << clientIp << ":" << clientPort << "] " << reqLine << " -> 403 Forbidden (directory: " << fullPath << ")" << endl;
            string body = "<html><body><h1>403 Forbidden</h1></body></html>";
            sendSimpleResponse(client, 403, "Forbidden", body);
            closesocket(client);
            continue;
        }

        ifstream ifs(fullPath, ios::binary);
        if (!ifs) {
            cout << "[" << clientIp << ":" << clientPort << "] " << reqLine << " -> 403 Forbidden (cannot open: " << fullPath << ")" << endl;
            string body = "<html><body><h1>403 Forbidden</h1></body></html>";
            sendSimpleResponse(client, 403, "Forbidden", body);
            closesocket(client);
            continue;
        }

        // 读取并发送文件
        ifs.seekg(0, ios::end);
        size_t fsize = ifs.tellg();
        ifs.seekg(0, ios::beg);
        string content;
        content.resize(fsize);
        ifs.read(&content[0], fsize);

        cout << "[" << clientIp << ":" << clientPort << "] " << reqLine << " -> 200 OK, Serving file: " << fullPath << " (" << fsize << " bytes)" << endl;

        string ctype = contentTypeByExt(fullPath);
        ostringstream header;
        header << "HTTP/1.1 200 OK\r\n";
        header << "Content-Type: " << ctype << "\r\n";
        header << "Content-Length: " << fsize << "\r\n";
        header << "Connection: close\r\n\r\n";
        string hdr = header.str();
        send(client, hdr.c_str(), (int)hdr.size(), 0);

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