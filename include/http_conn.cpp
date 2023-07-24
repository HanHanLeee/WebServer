#include "http_conn.h"

const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

const char* doc_root = "var/www/html";

int setnonblocking( int fd ) {
    int old_option = fcntl( fd, F_GETFL );
    int new_option = old_option | O_NONBLOCK;
    fcntl( fd, F_SETFL, new_option );
    return old_option;
}

void addfd( int epollfd, int fd, bool one_shot ) {
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP; 
    if( one_shot ) {
        event.events |= EPOLLONESHOT; // 仅监听一次事件
    }
    epoll_ctl( epollfd, EPOLL_CTL_ADD, fd, &event );
    setnonblocking( fd );
}

void removefd( int epollfd, int fd ) {
    epoll_ctl( epollfd, EPOLL_CTL_DEL, fd, 0 );
    close( fd );
}

void modfd( int epollfd, int fd, int ev ) {
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP; // 边缘触发
    epoll_ctl( epollfd, EPOLL_CTL_MOD, fd, &event );
}

int http_conn::m_user_count = 0; 
int http_conn::m_epollfd = -1;

void http_conn::close_conn( bool real_close ) {
    if( real_close && ( m_sockfd != -1 ) ) {
        removefd( m_epollfd, m_sockfd );
        m_sockfd = -1;
        m_user_count--; // 关闭连接时，将客户总量减1
    }
}

void http_conn::init(int sockfd, const sockaddr_in& addr)
{
    m_sockfd = sockfd;
    m_address = addr;
    // 以下两行是为了避免TIME_WAIT状态，仅用于调试，实际使用时应该去掉
    int reuse = 1;
    setsockopt( m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );
    addfd( m_epollfd, sockfd, true );
    m_user_count++; // 有新的连接时，将客户总量加1
    init();
}

void http_conn::init()
{
    m_check_state = CHECK_STATE_REQUESTLINE; // 初始状态为检查请求行
    m_linger = false; // 默认不保持连接
    m_method = GET; // 默认请求方法为GET
    m_url = 0; // 请求的目标文件的文件名
    m_version = 0; // HTTP协议版本号，仅支持HTTP/1.1
    m_content_length = 0; // HTTP请求的消息体的长度
    m_host = 0; // 主机名
    m_start_line = 0; // 当前正在解析的行的起始位置
    m_checked_idx = 0; // 当前正在分析的字符在读缓冲区中的位置
    m_read_idx = 0; // 读缓冲区中已经读入的客户数据的最后一个字节的下一个位置
    m_write_idx = 0; // 写缓冲区中待发送的字节数
    memset( m_read_buf, '\0', READ_BUFFER_SIZE ); // 读缓冲区
    memset( m_write_buf, '\0', WRITE_BUFFER_SIZE ); // 写缓冲区
    memset( m_real_file, '\0', FILENAME_LEN ); // 客户请求的目标文件的完整路径，其内容等于doc_root + m_url，doc_root是网站根目录
}

http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for(; m_checked_idx < m_read_idx; ++m_checked_idx) {
        temp = m_read_buf[m_checked_idx];
        if(temp == '\r') { // 如果当前字符是'\r'，则说明可能读取到一个完整的行
            if((m_checked_idx + 1) == m_read_idx) { // 如果'\r'是目前buffer中的最后一个已经被读入的客户数据，那么这次分析没有读取到一个完整的行，返回LINE_OPEN以表示还需要继续读取客户数据才能进一步分析
                return LINE_OPEN;
            } else if(m_read_buf[m_checked_idx + 1] == '\n') { // 如果下一个字符是'\n'，则说明我们成功读取到一个完整的行
                m_read_buf[m_checked_idx++] = '\0';  // 将'\r'和'\n'都替换成'\0'
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        } else if(temp == '\n') { // 如果当前字符是'\n'，也说明可能读取到一个完整的行
            if((m_checked_idx > 1) && (m_read_buf[m_checked_idx - 1] == '\r')) {  // 如果前一个字符是'\r'，则说明我们成功读取到一个完整的行
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if(temp == '\0') { // 如果当前字符是'\0'，说明客户发送的HTTP请求中存在语法问题
            return LINE_BAD;
        }
    }
    return LINE_OPEN; // 如果所有内容都分析完毕也没有遇到'\r'字符，则返回LINE_OPEN，表示还需要继续读取客户数据才能进一步分析
}

bool http_conn::read()
{
    if(m_read_idx >= READ_BUFFER_SIZE) { 
        return false;
    }

    int bytes_read = 0;
    while(true)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if(bytes_read == -1) {
            if(errno == EAGAIN || errno == EWOULDBLOCK) { 
                break;
            }
            return false;
        } else if(bytes_read == 0) {
            return false;
        }
        m_read_idx += bytes_read; 
    }
    return true;
}

http_conn::HTTP_CODE http_conn::parse_request_line(char* text)
{
    m_url = strpbrk(text, " \t"); // 在字符串text中找出第一个匹配字符串" \t"中任意字符的字符，并返回该字符的位置
    if(!m_url) {
        return BAD_REQUEST;
    }
    *m_url++ = '\0'; // 将该位置改为'\0'，以便将前面数据取出
    char* method = text;
    if(strcasecmp(method, "GET") == 0) { // 仅支持GET方法
        m_method = GET;
    } else {
        return BAD_REQUEST;
    }
    m_url += strspn(m_url, " \t"); // 跳过space和table字符，指向请求资源的第一个字符
    m_version = strpbrk(m_url, " \t"); // 在字符串m_url中找出第一个匹配字符串" \t"中任意字符的字符，并返回该字符的位置
    if(!m_version) {
        return BAD_REQUEST;
    }
    *m_version++ = '\0'; // 将该位置改为'\0'，以便将前面数据取出
    m_version += strspn(m_version, " \t"); // 跳过space和table字符，指向HTTP协议版本号
    if(strcasecmp(m_version, "HTTP/1.1") != 0) { // 仅支持HTTP/1.1
        return BAD_REQUEST;
    }
    if(strncasecmp(m_url, "http://", 7) == 0) { // 检查URL是否合法
        m_url += 7;
        m_url = strchr(m_url, '/'); // 在字符串m_url中找出第一个匹配字符'/'的字符，并返回该字符的位置
    }
    if(!m_url || m_url[0] != '/') {
        return BAD_REQUEST;
    }
    m_check_state = CHECK_STATE_HEADER; // 状态转移到检查头部字段
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::parse_headers(char* text)
{
    if(text[0] == '\0') { // 遇到空行，表示头部字段解析完毕
        if(m_content_length != 0) { // 如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，状态机转移到CHECK_STATE_CONTENT状态
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST; // 否则说明我们已经得到了一个完整的HTTP请求
    } else if(strncasecmp(text, "Connection:", 11) == 0) { // 处理Connection头部字段
        text += 11;
        text += strspn(text, " \t");
        if(strcasecmp(text, "keep-alive") == 0) { // 如果是keep-alive，则将linger标志设置为true
            m_linger = true;
        }
    } else if(strncasecmp(text, "Content-Length:", 15) == 0) { // 处理Content-Length头部字段
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    } else if(strncasecmp(text, "Host:", 5) == 0) { // 处理Host头部字段
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    } else { // 其他头部字段都不处理
        printf("oop! unknow header %s\n", text);
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::parse_content(char* text)
{
    if(m_read_idx >= (m_content_length + m_checked_idx)) { // 判断消息体是否完整读入
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK; // 记录当前行的读取状态
    HTTP_CODE ret = NO_REQUEST; // 记录HTTP请求的处理结果
    char* text = 0;

    while(((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK)) || ((line_status = parse_line()) == LINE_OK)) { // 循环读取和分析客户数据
        text = get_line(); // 获取读取的一行内容
        m_start_line = m_checked_idx; // 记录下一行的起始位置
        printf("got 1 http line: %s\n", text);

        switch(m_check_state) {
            case CHECK_STATE_REQUESTLINE: { // 第一个状态，分析请求行
                ret = parse_request_line(text);
                if(ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER: { // 第二个状态，分析头部字段
                ret = parse_headers(text);
                if(ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                } else if(ret == GET_REQUEST) {
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT: { // 第三个状态，分析消息体
                ret = parse_content(text);
                if(ret == GET_REQUEST) {
                    return do_request();
                }
                line_status = LINE_OPEN;
                break;
            }
            default: {
                return INTERNAL_ERROR;
            }
        }
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request()
{
    strcpy(m_real_file, doc_root); // 将根目录拷贝到m_real_file中
    int len = strlen(doc_root);
    strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1); // 将请求的资源路径拷贝到m_real_file中
    if(stat(m_real_file, &m_file_stat) < 0) { // 获取请求资源的文件属性，成功则将信息更新到m_file_stat结构体
        return NO_RESOURCE;
    }
    if(!(m_file_stat.st_mode & S_IROTH)) { // 判断文件的权限
        return FORBIDDEN_REQUEST;
    }
    if(S_ISDIR(m_file_stat.st_mode)) { // 判断文件类型，如果是目录，则返回BAD_REQUEST，表示请求报文有误
        return BAD_REQUEST;
    }
    int fd = open(m_real_file, O_RDONLY); // 以只读方式获取文件描述符
    m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0); // 通过mmap将该文件映射到内存地址m_file_address处，并告诉内核将文件内容更新到内存
    close(fd); // 避免文件描述符的浪费和占用
    return FILE_REQUEST;
}

void http_conn::unmap()
{
    if(m_file_address) {
        munmap(m_file_address, m_file_stat.st_size); // 释放mmap映射的内存
        m_file_address = 0;
    }
}

bool http_conn::write()
{
    int temp = 0; // 保存当前已经发送的字节数
    int bytes_have_send = 0; // 已经发送的字节数
    int bytes_to_send = m_write_idx; // 要发送的字节数为m_write_idx
    if(bytes_to_send == 0) { // 如果要发送的字节数为0，则表示响应报文为空，一般不会出现这种情况
        modfd(m_epollfd, m_sockfd, EPOLLIN); // 重新注册该事件，监听该事件上的读事件
        init();
        return true;
    }

    while(1)
    {
        temp = writev(m_sockfd, m_iv, m_iv_count); // 分散写
        if(temp <= -1)
        {
            if(errno == EAGAIN) // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，服务器无法立即接收到同一客户的下一个请求，但这可以保证连接的完整性
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap(); // 如果发送失败，则取消映射
            return false;
        }

        bytes_to_send -= temp; // 更新已发送字节数
        bytes_have_send += temp;
        // 
        if(bytes_to_send <= bytes_have_send) 
        {
            unmap(); // 取消文件内存映射
            if(m_linger) // 如果是长连接，则重新初始化HTTP对象，继续使用该连接
            {
                init();
                modfd(m_epollfd, m_sockfd, EPOLLIN);
                return true;
            }
            else // 如果是短连接，则关闭连接
            {
                modfd(m_epollfd, m_sockfd, EPOLLIN);
                return false;
            }
        }
    }
}

bool http_conn::add_response(const char* format, ...)
{
    if(m_write_idx >= WRITE_BUFFER_SIZE) { // 如果写入的HTTP响应报文超出了写缓冲区，则报错
        return false;
    }
    va_list arg_list; // 定义可变参数列表
    va_start(arg_list, format); // 将变量arg_list初始化为传入参数
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list); // 将数据从可变参数列表写入缓冲区
    if(len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx)) { // 如果写入的数据长度超过缓冲区剩余空间，则报错
        return false;
    }
    m_write_idx += len; // 更新m_write_idx位置
    va_end(arg_list); // 清空可变参数列表
    return true;
}

bool http_conn::add_status_line(int status, const char* title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title); // 添加状态行
}

bool http_conn::add_headers(int content_len)
{
    add_content_length(content_len); 
    add_linger(); // 添加Connection，表示是否为长连接
    add_blank_line(); // 添加空行
}

bool http_conn::add_content_length(int content_len)
{
    // 通过add_response添加文本，添加文本的长度等于content_len
    return add_response("Content-Length: %d\r\n", content_len); 
}

bool http_conn::add_linger()
{
    // 如果是长连接，则添加http头部Connection的值为keep-alive
    return add_response("Connection: %s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n"); // 添加空行
}

bool http_conn::add_content(const char* content)
{
    return add_response("%s", content); // 添加文本content
}

bool http_conn::process_write(HTTP_CODE ret)
{
    switch(ret)
    {
        case INTERNAL_ERROR: // 内部错误，500
        {
            add_status_line(500, error_500_title); // 添加状态行
            add_headers(strlen(error_500_form)); // 添加消息报头
            if(!add_content(error_500_form)) { // 添加消息报体
                return false;
            }
            break;
        }
        case BAD_REQUEST: // 请求报文语法错误，400
        {
            add_status_line(400, error_400_title); // 添加状态行
            add_headers(strlen(error_400_form)); // 添加消息报头
            if(!add_content(error_400_form)) { // 添加消息报体
                return false;
            }
            break;
        }
        case FORBIDDEN_REQUEST: // 请求资源禁止访问，403
        {
            add_status_line(403, error_403_title); // 添加状态行
            add_headers(strlen(error_403_form)); // 添加消息报头
            if(!add_content(error_403_form)) { // 添加消息报体
                return false;
            }
            break;
    
        }
        case NO_RESOURCE: // 请求资源不存在，404
        {
            add_status_line(404, error_404_title); // 添加状态行
            add_headers(strlen(error_404_form)); // 添加消息报头
            if(!add_content(error_404_form)) { // 添加消息报体
                return false;
            }
            break;
        }
        case FILE_REQUEST: // 请求资源可以访问，200
        {
            add_status_line(200, ok_200_title); // 添加状态行
            if(m_file_stat.st_size != 0) { // 如果请求的资源存在
                add_headers(m_file_stat.st_size); // 添加消息报头
                m_iv[0].iov_base = m_write_buf; // 第一个iovec指针指向响应报文缓冲区m_write_buf
                m_iv[0].iov_len = m_write_idx; // 第一个iovec指针长度指向响应报文长度
                m_iv[1].iov_base = m_file_address; // 第二个iovec指针指向mmap返回的文件指针
                m_iv[1].iov_len = m_file_stat.st_size; // 第二个iovec指针长度指向文件大小
                m_iv_count = 2; // 设置写入内存的iovec的数量为2，写内存的数据为响应报文头部信息和文件
                return true;
            }
            else { // 如果请求的资源大小为0，则返回空白html文件
                const char* ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if(!add_content(ok_string)) {
                    return false;
                }
            }
        }
        default:
        {
            return false;
        }
    }
    // 除FILE_REQUEST状态外，其余状态只申请一个iovec，指向响应报文缓冲区
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1; // 设置写入内存的iovec的数量为1
    return true;
}

void http_conn::process()
{
    HTTP_CODE read_ret = process_read(); // 调用process_read完成报文解析
    if(read_ret == NO_REQUEST) { // 如果请求不完整，则注册并监听读事件，以等待下一次数据到来
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }

    bool write_ret = process_write(read_ret); // 调用process_write完成报文响应
    if(!write_ret) { // 如果报文响应失败，则关闭连接
        close_conn();
    }

    modfd(m_epollfd, m_sockfd, EPOLLOUT); // 注册并监听写事件
}