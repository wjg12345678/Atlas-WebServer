#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include "log.h"
#include <pthread.h>
using namespace std;

Log::Log()
{
    m_count = 0;
    m_is_async = false;
    m_log_queue = NULL;
    m_buf = NULL;
    m_fp = NULL;
    m_write_thread = 0;
    m_log_level = INFO;
    m_today_year = 0;
    m_today_mon = 0;
    m_today = 0;
    m_file_index = 0;
    memset(dir_name, '\0', sizeof(dir_name));
    memset(log_name, '\0', sizeof(log_name));
}

Log::~Log()
{
    if (m_log_queue != NULL)
    {
        delete m_log_queue;
        m_log_queue = NULL;
    }
    if (m_buf != NULL)
    {
        delete[] m_buf;
        m_buf = NULL;
    }
    if (m_fp != NULL)
    {
        fflush(m_fp);
        fclose(m_fp);
        m_fp = NULL;
    }
}
//异步需要设置阻塞队列的长度，同步不需要设置
bool Log::init(const char *file_name, int close_log, int log_buf_size, int split_lines, int max_queue_size, int log_level)
{
    //如果设置了max_queue_size,则设置为异步
    if (max_queue_size >= 1)
    {
        m_is_async = true;
        m_log_queue = new block_queue<string>(max_queue_size);
        //flush_log_thread为回调函数,这里表示创建线程异步写日志
        pthread_create(&m_write_thread, NULL, flush_log_thread, NULL);
        pthread_detach(m_write_thread);
    }
    
    m_close_log = close_log;
    m_log_level = log_level;
    m_log_buf_size = log_buf_size;
    m_buf = new char[m_log_buf_size];
    memset(m_buf, '\0', m_log_buf_size);
    m_split_lines = split_lines;

    time_t t = time(NULL);
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;

 
    const char *p = strrchr(file_name, '/');
    char log_full_name[256] = {0};

    if (p == NULL)
    {
        snprintf(log_full_name, 255, "%d_%02d_%02d_%s", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, file_name);
    }
    else
    {
        strcpy(log_name, p + 1);
        strncpy(dir_name, file_name, p - file_name + 1);
        snprintf(log_full_name, 255, "%s%d_%02d_%02d_%s", dir_name, my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, log_name);
    }

    m_today_year = my_tm.tm_year + 1900;
    m_today_mon = my_tm.tm_mon + 1;
    m_today = my_tm.tm_mday;
    m_file_index = 0;
    
    m_fp = fopen(log_full_name, "a");
    if (m_fp == NULL)
    {
        return false;
    }

    return true;
}

const char *Log::level_name(int level) const
{
    switch (level)
    {
    case DEBUG:
        return "[debug]:";
    case INFO:
        return "[info]:";
    case WARN:
        return "[warn]:";
    case ERROR:
        return "[error]:";
    default:
        return "[info]:";
    }
}

void Log::rotate_file(const tm &my_tm)
{
    char new_log[256] = {0};
    fflush(m_fp);
    fclose(m_fp);

    bool date_changed = (m_today_year != my_tm.tm_year + 1900) ||
                        (m_today_mon != my_tm.tm_mon + 1) ||
                        (m_today != my_tm.tm_mday);

    if (date_changed)
    {
        m_today_year = my_tm.tm_year + 1900;
        m_today_mon = my_tm.tm_mon + 1;
        m_today = my_tm.tm_mday;
        m_file_index = 0;
        m_count = 0;
    }
    else
    {
        ++m_file_index;
    }

    if (m_file_index == 0)
    {
        snprintf(new_log, 255, "%s%d_%02d_%02d_%s", dir_name, m_today_year, m_today_mon, m_today, log_name);
    }
    else
    {
        snprintf(new_log, 255, "%s%d_%02d_%02d_%s.%d", dir_name, m_today_year, m_today_mon, m_today, log_name, m_file_index);
    }
    m_fp = fopen(new_log, "a");
}

void Log::write_log(int level, const char *format, ...)
{
    if (level < m_log_level)
    {
        return;
    }

    struct timeval now = {0, 0};
    gettimeofday(&now, NULL);
    time_t t = now.tv_sec;
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;
    const char *s = level_name(level);
    //写入一个log，对m_count++, m_split_lines最大行数
    m_mutex.lock();
    m_count++;

    if ((m_today_year != my_tm.tm_year + 1900) ||
        (m_today_mon != my_tm.tm_mon + 1) ||
        (m_today != my_tm.tm_mday) ||
        (m_split_lines > 0 && m_count > 0 && m_count % m_split_lines == 0))
    {
        rotate_file(my_tm);
    }
 
    m_mutex.unlock();

    va_list valst;
    va_start(valst, format);

    string log_str;
    m_mutex.lock();

    //写入的具体时间内容格式
    int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                     my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
                     my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, now.tv_usec, s);
    
    int m = vsnprintf(m_buf + n, m_log_buf_size - n - 1, format, valst);
    m_buf[n + m] = '\n';
    m_buf[n + m + 1] = '\0';
    log_str = m_buf;

    m_mutex.unlock();

    if (m_is_async && m_log_queue != NULL && !m_log_queue->full())
    {
        m_log_queue->push(log_str);
    }
    else
    {
        m_mutex.lock();
        fputs(log_str.c_str(), m_fp);
        m_mutex.unlock();
    }

    va_end(valst);
}

void Log::flush(void)
{
    if (m_is_async)
    {
        return;
    }
    m_mutex.lock();
    //强制刷新写入流缓冲区
    fflush(m_fp);
    m_mutex.unlock();
}
