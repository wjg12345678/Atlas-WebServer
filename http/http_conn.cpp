#include "http_conn.h"

#include <mysql/mysql.h>
#include <fstream>
#include <openssl/err.h>

#include "../memorypool/memory_pool.h"

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_501_title = "Not Implemented";
const char *error_501_form = "The requested HTTP feature is not implemented.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock;
map<string, string> users;
SSL_CTX *http_conn::m_ssl_ctx = NULL;
bool http_conn::m_tls_enabled = false;

void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

//对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;

void http_conn::configure_tls(SSL_CTX *ssl_ctx, bool https_enabled)
{
    m_ssl_ctx = ssl_ctx;
    m_tls_enabled = https_enabled && ssl_ctx != NULL;
}

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        close_file();
        if (m_ssl != NULL)
        {
            SSL_shutdown(m_ssl);
            SSL_free(m_ssl);
            m_ssl = NULL;
        }
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化连接,外部调用初始化套接字地址
void http_conn::init(int epollfd, int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
    m_epollfd = epollfd;
    m_sockfd = sockfd;
    m_address = addr;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;
    refresh_active();
    m_https_enabled = m_tls_enabled;
    m_tls_handshake_done = !m_https_enabled;
    m_tls_want_event = EPOLLIN;
    if (m_https_enabled)
    {
        m_ssl = SSL_new(m_ssl_ctx);
        if (m_ssl != NULL)
        {
            SSL_set_fd(m_ssl, m_sockfd);
            SSL_set_accept_state(m_ssl);
            SSL_set_mode(m_ssl, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
        }
        else
        {
            m_https_enabled = false;
            m_tls_handshake_done = true;
        }
    }
    else
    {
        m_ssl = NULL;
    }

    addfd(m_epollfd, sockfd, true, TRIGMode);
    m_user_count++;

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

void http_conn::refresh_active()
{
    m_last_active = time(NULL);
}

void http_conn::reset_ring_buffer(ring_buffer &ring, char *storage, int capacity)
{
    ring.buffer = storage;
    ring.capacity = capacity;
    ring.head = 0;
    ring.tail = 0;
    ring.size = 0;
}

int http_conn::ring_writable(const ring_buffer &ring) const
{
    return ring.capacity - ring.size;
}

int http_conn::ring_append(ring_buffer &ring, const char *data, int len)
{
    if (len > ring_writable(ring))
    {
        return -1;
    }

    int first = ring.capacity - ring.tail;
    if (first > len)
    {
        first = len;
    }
    memcpy(ring.buffer + ring.tail, data, first);
    memcpy(ring.buffer, data + first, len - first);
    ring.tail = (ring.tail + len) % ring.capacity;
    ring.size += len;
    return len;
}

int http_conn::ring_recv(ring_buffer &ring)
{
    if (ring.size >= ring.capacity)
    {
        return -1;
    }

    int writable = ring_writable(ring);
    int first = ring.capacity - ring.tail;
    if (first > writable)
    {
        first = writable;
    }

    int bytes_read = 0;
    if (m_https_enabled)
    {
        ERR_clear_error();
        bytes_read = SSL_read(m_ssl, ring.buffer + ring.tail, first);
        if (bytes_read <= 0)
        {
            int ssl_error = SSL_get_error(m_ssl, bytes_read);
            if (ssl_error == SSL_ERROR_WANT_READ)
            {
                m_tls_want_event = EPOLLIN;
                errno = EAGAIN;
            }
            else if (ssl_error == SSL_ERROR_WANT_WRITE)
            {
                m_tls_want_event = EPOLLOUT;
                errno = EAGAIN;
            }
            else if (ssl_error == SSL_ERROR_ZERO_RETURN)
            {
                return 0;
            }
            return -1;
        }
    }
    else
    {
        bytes_read = recv(m_sockfd, ring.buffer + ring.tail, first, 0);
    }
    if (bytes_read <= 0)
    {
        return bytes_read;
    }

    ring.tail = (ring.tail + bytes_read) % ring.capacity;
    ring.size += bytes_read;
    return bytes_read;
}

int http_conn::ring_peek(const ring_buffer &ring, char *dest, int len) const
{
    if (len > ring.size)
    {
        len = ring.size;
    }
    int first = ring.capacity - ring.head;
    if (first > len)
    {
        first = len;
    }
    memcpy(dest, ring.buffer + ring.head, first);
    memcpy(dest + first, ring.buffer, len - first);
    return len;
}

int http_conn::ring_send(ring_buffer &ring)
{
    if (ring.size == 0)
    {
        return 0;
    }

    int first = ring.capacity - ring.head;
    if (first > ring.size)
    {
        first = ring.size;
    }

    int bytes_sent = socket_send(ring.buffer + ring.head, first);
    if (bytes_sent <= 0)
    {
        return bytes_sent;
    }

    ring.head = (ring.head + bytes_sent) % ring.capacity;
    ring.size -= bytes_sent;
    if (ring.size == 0)
    {
        ring.head = 0;
        ring.tail = 0;
    }
    return bytes_sent;
}

int http_conn::socket_send(const char *buffer, int len)
{
    if (!m_https_enabled)
    {
        return send(m_sockfd, buffer, len, 0);
    }

    ERR_clear_error();
    int bytes_sent = SSL_write(m_ssl, buffer, len);
    if (bytes_sent > 0)
    {
        return bytes_sent;
    }

    int ssl_error = SSL_get_error(m_ssl, bytes_sent);
    if (ssl_error == SSL_ERROR_WANT_READ)
    {
        m_tls_want_event = EPOLLIN;
        errno = EAGAIN;
        return -1;
    }
    if (ssl_error == SSL_ERROR_WANT_WRITE)
    {
        m_tls_want_event = EPOLLOUT;
        errno = EAGAIN;
        return -1;
    }
    if (ssl_error == SSL_ERROR_ZERO_RETURN)
    {
        return 0;
    }

    return -1;
}

int http_conn::wait_event_for_io(int default_event) const
{
    if (m_https_enabled)
    {
        return m_tls_want_event;
    }
    return default_event;
}

bool http_conn::send_file_over_tls()
{
    while (true)
    {
        if (m_file_send_offset >= m_file_send_size)
        {
            ssize_t file_read = read(m_filefd, m_file_send_buf, sizeof(m_file_send_buf));
            if (file_read < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                close_file();
                return false;
            }
            if (file_read == 0)
            {
                close_file();
                return true;
            }
            m_file_send_offset = 0;
            m_file_send_size = file_read;
        }

        int bytes_sent = socket_send(m_file_send_buf + m_file_send_offset, m_file_send_size - m_file_send_offset);
        if (bytes_sent < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                modfd(m_epollfd, m_sockfd, wait_event_for_io(EPOLLOUT), m_TRIGMode);
                refresh_active();
                return true;
            }
            close_file();
            return false;
        }
        if (bytes_sent == 0)
        {
            close_file();
            return false;
        }

        m_file_send_offset += bytes_sent;
        bytes_have_send += bytes_sent;
        bytes_to_send -= bytes_sent;

        if (m_file_send_offset >= m_file_send_size)
        {
            m_file_send_offset = 0;
            m_file_send_size = 0;
        }

        if (bytes_to_send <= 0)
        {
            close_file();
            return true;
        }
    }
}

void http_conn::sync_read_buffer()
{
    ring_peek(m_read_ring, m_read_buf, m_read_ring.size);
    m_read_idx = m_read_ring.size;
    if (m_read_idx < READ_BUFFER_SIZE)
    {
        m_read_buf[m_read_idx] = '\0';
    }
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_header_bytes_sent = 0;
    m_file_offset = 0;
    m_filefd = -1;
    m_response_body = NULL;
    m_response_body_len = 0;
    m_file_send_offset = 0;
    m_file_send_size = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_query_string = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_is_http_1_1 = false;
    m_chunked = false;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    memset(m_read_ring_buf, '\0', READ_BUFFER_SIZE);
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_ring_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
    memset(m_content_type, '\0', sizeof(m_content_type));
    memset(m_file_send_buf, '\0', sizeof(m_file_send_buf));
    reset_ring_buffer(m_read_ring, m_read_ring_buf, READ_BUFFER_SIZE);
    reset_ring_buffer(m_write_ring, m_write_ring_buf, WRITE_BUFFER_SIZE);
}

int http_conn::do_tls_handshake()
{
    if (!m_https_enabled || m_tls_handshake_done)
    {
        return 1;
    }

    ERR_clear_error();
    int ret = SSL_accept(m_ssl);
    if (ret == 1)
    {
        m_tls_handshake_done = true;
        m_tls_want_event = EPOLLIN;
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        refresh_active();
        return 1;
    }

    int ssl_error = SSL_get_error(m_ssl, ret);
    if (ssl_error == SSL_ERROR_WANT_READ)
    {
        m_tls_want_event = EPOLLIN;
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return 0;
    }
    if (ssl_error == SSL_ERROR_WANT_WRITE)
    {
        m_tls_want_event = EPOLLOUT;
        modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
        return 0;
    }

    return -1;
}

//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')
        {
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n')
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{
    if (ring_writable(m_read_ring) <= 0)
    {
        return false;
    }
    int bytes_read = 0;

    //LT读取数据
    if (0 == m_TRIGMode)
    {
        bytes_read = ring_recv(m_read_ring);
        if (bytes_read > 0)
        {
            sync_read_buffer();
            refresh_active();
            return true;
        }

        if (bytes_read == 0)
        {
            return false;
        }

        if (errno == EINTR)
        {
            return read_once();
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            modfd(m_epollfd, m_sockfd, wait_event_for_io(EPOLLIN), m_TRIGMode);
            return true;
        }

        return false;
    }
    //ET读数据
    return read_once_et();
}

bool http_conn::read_once_et()
{
    bool has_read = false;

    while (ring_writable(m_read_ring) > 0)
    {
        int bytes_read = ring_recv(m_read_ring);
        if (bytes_read > 0)
        {
            has_read = true;
            continue;
        }

        if (bytes_read == 0)
        {
            return false;
        }

        if (errno == EINTR)
        {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            if (has_read)
            {
                sync_read_buffer();
                refresh_active();
            }
            modfd(m_epollfd, m_sockfd, wait_event_for_io(EPOLLIN), m_TRIGMode);
            return has_read;
        }

        return false;
    }

    if (0 == m_close_log)
    {
        Log::get_instance()->write_log(2, "read buffer is full in ET mode, sockfd=%d", m_sockfd);
        Log::get_instance()->flush();
    }
    sync_read_buffer();
    return false;
}

//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';
    char *method = text;
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "HEAD") == 0)
        m_method = HEAD;
    else if (strcasecmp(method, "OPTIONS") == 0)
        m_method = OPTIONS;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else if (strcasecmp(method, "PUT") == 0)
        m_method = PUT;
    else if (strcasecmp(method, "DELETE") == 0)
        m_method = DELETE;
    else if (strcasecmp(method, "TRACE") == 0)
        m_method = TRACE;
    else if (strcasecmp(method, "CONNECT") == 0)
        m_method = CONNECT;
    else if (strcasecmp(method, "PATCH") == 0)
        m_method = PATH;
    else
        return BAD_REQUEST;
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") == 0)
    {
        m_is_http_1_1 = true;
        m_linger = true;
    }
    else if (strcasecmp(m_version, "HTTP/1.0") == 0)
    {
        m_is_http_1_1 = false;
        m_linger = false;
    }
    else
        return BAD_REQUEST;
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    m_query_string = strchr(m_url, '?');
    if (m_query_string)
    {
        *m_query_string++ = '\0';
    }
    //当url为/时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    if (text[0] == '\0')
    {
        if (m_is_http_1_1 && (!m_host || m_host[0] == '\0'))
        {
            return BAD_REQUEST;
        }
        if (m_chunked)
        {
            return NOT_IMPLEMENTED;
        }
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
        else if (strcasecmp(text, "close") == 0)
        {
            m_linger = false;
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
        if (m_content_length < 0)
        {
            return BAD_REQUEST;
        }
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else if (strncasecmp(text, "Content-Type:", 13) == 0)
    {
        text += 13;
        text += strspn(text, " \t");
        strncpy(m_content_type, text, sizeof(m_content_type) - 1);
    }
    else if (strncasecmp(text, "Transfer-Encoding:", 18) == 0)
    {
        text += 18;
        text += strspn(text, " \t");
        if (strcasecmp(text, "chunked") == 0)
        {
            m_chunked = true;
        }
    }
    else
    {
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}

//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_chunked)
    {
        return NOT_IMPLEMENTED;
    }
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        //POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();
        m_start_line = m_checked_idx;
        LOG_INFO("%s", text);
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST || ret == NOT_IMPLEMENTED)
                return ret;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST || ret == NOT_IMPLEMENTED)
                return ret;
            else if (ret == GET_REQUEST)
            {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);
            if (ret == NOT_IMPLEMENTED)
                return ret;
            if (ret == GET_REQUEST)
                return do_request();
            line_status = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request()
{
    if (m_method == OPTIONS)
    {
        m_response_body = "";
        m_response_body_len = 0;
        return OPTIONS_REQUEST;
    }

    if (!(m_method == GET || m_method == POST || m_method == HEAD))
    {
        return NOT_IMPLEMENTED;
    }

    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    //printf("m_url:%s\n", m_url);
    const char *p = strrchr(m_url, '/');

    //处理cgi
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        MemoryPoolBuffer url_real_buffer;
        char *m_url_real = url_real_buffer.get();
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);

        //将用户名和密码提取出来
        //user=123&passwd=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            MemoryPoolBuffer sql_insert_buffer;
            char *sql_insert = sql_insert_buffer.get();
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end())
            {
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res)
                    strcpy(m_url, "/log.html");
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }

    if (*(p + 1) == '0')
    {
        MemoryPoolBuffer url_real_buffer;
        char *m_url_real = url_real_buffer.get();
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
    }
    else if (*(p + 1) == '1')
    {
        MemoryPoolBuffer url_real_buffer;
        char *m_url_real = url_real_buffer.get();
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
    }
    else if (*(p + 1) == '5')
    {
        MemoryPoolBuffer url_real_buffer;
        char *m_url_real = url_real_buffer.get();
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
    }
    else if (*(p + 1) == '6')
    {
        MemoryPoolBuffer url_real_buffer;
        char *m_url_real = url_real_buffer.get();
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
    }
    else if (*(p + 1) == '7')
    {
        MemoryPoolBuffer url_real_buffer;
        char *m_url_real = url_real_buffer.get();
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
    }
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    m_filefd = open(m_real_file, O_RDONLY);
    if (m_filefd < 0)
        return NO_RESOURCE;
    return FILE_REQUEST;
}

void http_conn::close_file()
{
    if (m_filefd != -1)
    {
        close(m_filefd);
        m_filefd = -1;
    }
}
bool http_conn::write()
{
    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, wait_event_for_io(EPOLLIN), m_TRIGMode);
        init();
        refresh_active();
        return true;
    }

    while (1)
    {
        if (m_write_ring.size > 0)
        {
            int header_written = ring_send(m_write_ring);
            if (header_written < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    modfd(m_epollfd, m_sockfd, wait_event_for_io(EPOLLOUT), m_TRIGMode);
                    refresh_active();
                    return true;
                }
                close_file();
                return false;
            }
            if (header_written == 0)
            {
                close_file();
                return false;
            }

            m_header_bytes_sent += header_written;
            bytes_have_send += header_written;
            bytes_to_send -= header_written;
            if (bytes_to_send <= 0)
            {
                close_file();
                modfd(m_epollfd, m_sockfd, wait_event_for_io(EPOLLIN), m_TRIGMode);
                refresh_active();

                if (m_linger)
                {
                    init();
                    return true;
                }
                return false;
            }
            continue;
        }

        if (m_filefd != -1)
        {
            if (m_https_enabled)
            {
                if (!send_file_over_tls())
                {
                    return false;
                }
                if (bytes_to_send > 0)
                {
                    return true;
                }
            }
            else
            {
                ssize_t file_written = sendfile(m_sockfd, m_filefd, &m_file_offset, bytes_to_send);
                if (file_written < 0)
                {
                    if (errno == EINTR)
                    {
                        continue;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                    {
                        modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                        refresh_active();
                        return true;
                    }
                    close_file();
                    return false;
                }
                if (file_written == 0)
                {
                    close_file();
                    return false;
                }

                bytes_have_send += file_written;
                bytes_to_send -= file_written;
            }
        }

        if (bytes_to_send <= 0)
        {
            close_file();
            modfd(m_epollfd, m_sockfd, wait_event_for_io(EPOLLIN), m_TRIGMode);
            refresh_active();

            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}
bool http_conn::add_response(const char *format, ...)
{
    va_list arg_list;
    va_start(arg_list, format);
    char temp_buf[WRITE_BUFFER_SIZE];
    int len = vsnprintf(temp_buf, sizeof(temp_buf), format, arg_list);
    if (len < 0 || len >= (int)sizeof(temp_buf))
    {
        va_end(arg_list);
        return false;
    }
    va_end(arg_list);

    if (ring_append(m_write_ring, temp_buf, len) < 0)
    {
        return false;
    }
    m_write_idx = m_write_ring.size;
    ring_peek(m_write_ring, m_write_buf, m_write_ring.size);
    if (m_write_ring.size < WRITE_BUFFER_SIZE)
    {
        m_write_buf[m_write_ring.size] = '\0';
    }

    LOG_INFO("request:%s", temp_buf);

    return true;
}
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
bool http_conn::add_headers(int content_len, const char *content_type)
{
    return add_content_length(content_len) && add_content_type(content_type) &&
           add_linger() && add_keep_alive() && add_allow() &&
           add_blank_line();
}
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}
bool http_conn::add_content_type(const char *content_type)
{
    return add_response("Content-Type:%s\r\n", content_type);
}
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
bool http_conn::add_keep_alive()
{
    if (!m_linger)
    {
        return true;
    }
    return add_response("Keep-Alive:%s\r\n", "timeout=15, max=100");
}
bool http_conn::add_allow()
{
    return add_response("Allow:%s\r\n", "GET, POST, HEAD, OPTIONS");
}
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    case BAD_REQUEST:
    {
        add_status_line(400, error_400_title);
        add_headers(strlen(error_400_form), "text/plain");
        if (!add_content(error_400_form))
            return false;
        break;
    }
    case NO_RESOURCE:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form), "text/plain");
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form), "text/plain");
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case NOT_IMPLEMENTED:
    {
        add_status_line(501, error_501_title);
        add_headers(strlen(error_501_form), "text/plain");
        if (!add_content(error_501_form))
            return false;
        break;
    }
    case OPTIONS_REQUEST:
    {
        add_status_line(204, ok_200_title);
        add_headers(0, "text/plain");
        bytes_to_send = m_write_idx;
        m_header_bytes_sent = 0;
        m_file_offset = 0;
        return true;
    }
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        if (m_method == HEAD)
        {
            add_headers(m_file_stat.st_size);
            close_file();
            bytes_to_send = m_write_idx;
            m_header_bytes_sent = 0;
            m_file_offset = 0;
            return true;
        }
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            m_header_bytes_sent = 0;
            m_file_offset = 0;
            return true;
        }
        else
        {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
        break;
    }
    default:
        return false;
    }
    bytes_to_send = m_write_idx;
    m_header_bytes_sent = 0;
    m_file_offset = 0;
    return true;
}
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
        return;
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}
