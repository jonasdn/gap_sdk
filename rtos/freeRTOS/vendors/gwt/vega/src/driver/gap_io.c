/*
 * Copyright (c) 2019, GreenWaves Technologies, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * o Redistributions of source code must retain the above copyright notice, this list
 *   of conditions and the following disclaimer.
 *
 * o Redistributions in binary form must reproduce the above copyright notice, this
 *   list of conditions and the following disclaimer in the documentation and/or
 *   other materials provided with the distribution.
 *
 * o Neither the name of GreenWaves Technologies, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdarg.h>
#include "pmsis.h"
#include "gap_common.h"

#if defined(FEATURE_CLUSTER)
#include "pmsis/cluster/cluster_sync/cl_synchronisation.h"
#include "pmsis/cluster/cluster_sync/fc_to_cl_delegate.h"

extern struct cluster_driver_data *__per_cluster_data[];

/* L1 printf lock pointer. */
extern char __printf_lock_ptr_l1;
#endif  /* FEATURE_CLUSTER */

/* TAS offset. vega : TAS in L2. */
#define PRINTF_TAS_L1
#define PRINTF_TAS_L2_OFFSET     ( 25 )
#define PRINTF_TAS_L1_OFFSET     ( 20 )

/*
 * This should be used in case of printf via uart before scheduler has started.
 * Output will be on terminal instead of uart. After scheduler has started, output
 * will be via uart.
 */
uint8_t g_freertos_scheduler_started = 0;

/*
 * OS Mutex used to synchronize printf when only OS tasks are running and on FC.
 */
SemaphoreHandle_t g_printf_mutex = NULL;

/* L2 printf lock pointer. */
extern char __printf_lock_ptr_l2;

/* TAS synchronization between FC and Cluster, when Cluster is powered on. */
static inline uint32_t __tas_addr_lock(uint32_t addr, uint32_t offset)
{
    uint32_t value = 0;
    hal_compiler_barrier();
    value = *((volatile uint32_t *) (addr + (1 << offset)));
    hal_compiler_barrier();
    return value;
}

static inline void __tas_addr_unlock(uint32_t addr)
{
    *((volatile uint32_t *) addr) = 0;
}

static inline uint32_t __tas_lock()
{
    #if defined(PRINTF_TAS_L2)
    return __tas_addr_lock((uint32_t) &__printf_lock_ptr_l2, (uint32_t) PRINTF_TAS_L2_OFFSET);
    #else
    return __tas_addr_lock((uint32_t) &__printf_lock_ptr_l1, (uint32_t) PRINTF_TAS_L1_OFFSET);
    #endif  /* PRINTF_L2 */
}

static inline void __tas_unlock()
{
    #if defined(PRINTF_TAS_L2)
    __tas_addr_unlock((uint32_t) &__printf_lock_ptr_l2);
    #else
    __tas_addr_unlock((uint32_t) &__printf_lock_ptr_l1);
    #endif  /* PRINTF_TAS_L2 */
}

#if defined(PRINTF_UART)

static uint8_t g_printf_uart_index = 0;
PI_L2 static char g_printf_uart_buffer[PRINTF_BUFFER_SIZE];

struct pi_device g_printf_uart_dev = {0};

void printf_uart_init(uint8_t uart_id)
{
    struct pi_uart_conf config = {0};
    /* Init & open uart. */
    pi_uart_conf_init(&config);
    config.uart_id = uart_id;
    config.baudrate_bps = 115200;
    config.enable_tx = 1;
    config.enable_rx = 0;
    pi_open_from_conf(&g_printf_uart_dev, &config);

    if (pi_uart_open(&g_printf_uart_dev))
    {
        pmsis_exit(-117);
    }
    g_printf_uart_index = 0;
}

static void __uart_putc(char c)
{
    #if defined(FEATURE_CLUSTER)
    if (!__native_is_fc())
    {
        uint8_t core_id = pi_core_id();
        uint8_t cluster_id = pi_cluster_id();
        uint8_t *buffer = &(__per_cluster_data[cluster_id]->printf_buffer[core_id * PRINTF_BUFFER_SIZE]);
        uint32_t *index = &(__per_cluster_data[cluster_id]->printf_buffer_index[core_id]);
        buffer[*index] = c;
        (*index)++;
        if ((*index == ((uint32_t) PRINTF_BUFFER_SIZE - 1)) || (c == '\n'))
        {
            pi_cl_uart_req_t req = {0};
            pi_cl_uart_write(&g_printf_uart_dev, (void *) buffer, *index, &req);
            pi_cl_uart_write_wait(&req);
            *index = 0;
            //memset(buffer, 0, (uint32_t) PRINTF_BUFFER_SIZE);
        }
    }
    else
    #endif  /* FEATURE_CLUSTER */
    {
        g_printf_uart_buffer[g_printf_uart_index] = c;
        g_printf_uart_index++;
        if ((g_printf_uart_index == ((uint32_t) PRINTF_BUFFER_SIZE - 1)) ||
            (c == '\n'))
        {
            pi_uart_write(&g_printf_uart_dev, (void *) g_printf_uart_buffer,
                          g_printf_uart_index);
            g_printf_uart_index = 0;
            //memset(g_printf_semihost_buffer, 0, (uint32_t) PRINTF_BUFFER_SIZE);
        }
    }
}

static void __uart_printf_flush(char c)
{
    if ((g_printf_uart_index > 0) &&
        (g_printf_uart_index < (uint32_t) PRINTF_BUFFER_SIZE))
    {
        g_printf_uart_buffer[g_printf_uart_index++] = c;
        pi_uart_write(&g_printf_uart_dev, (void *) g_printf_uart_buffer,
                      g_printf_uart_index);
    }
    pi_uart_close(&g_printf_uart_dev);
}

#elif defined(PRINTF_SEMIHOST)  /* PRINTF_UART */
#include "semihost.h"

static uint8_t g_printf_semihost_index = 0;
static char g_printf_semihost_buffer[PRINTF_BUFFER_SIZE];

struct semihost_putc_req_s
{
    char *buffer;               /*!< Buffer to send. */
    uint32_t size;              /*!< Buffer size. */
    uint8_t done;               /*!< Variable to check completion. */
    uint8_t cid;                /*!< Cluster ID. */
    pi_task_t cb;               /*!< Callback function. */
};

static void __semihost_buffer_write_exec(char *buffer, uint32_t size)
{
    semihost_write0((const char *) buffer);
}

static void __semihost_putc_cluster_req(void *arg)
{
    struct semihost_putc_req_s *req = (struct semihost_putc_req_s *) arg;
    __semihost_buffer_write_exec(req->buffer, req->size);
    cl_notify_task_done(&(req->done), req->cid);
}

static void __semihost_putc(char c)
{
    #if defined(FEATURE_CLUSTER)
    if (!__native_is_fc())
    {
        uint8_t core_id = pi_core_id();
        uint8_t cluster_id = pi_cluster_id();
        uint8_t *buffer = &(__per_cluster_data[cluster_id]->printf_buffer[core_id * PRINTF_BUFFER_SIZE]);
        uint32_t *index = &(__per_cluster_data[cluster_id]->printf_buffer_index[core_id]);
        buffer[*index] = c;
        (*index)++;
        if ((*index == ((uint32_t) PRINTF_BUFFER_SIZE - 1)) || (c == '\n'))
        {
            struct semihost_putc_req_s req = {0};
            req.buffer = (char *) buffer;
            req.size = *index;
            req.done = 0;
            req.cid = __native_cluster_id();
            pi_task_callback(&(req.cb), (void *) __semihost_putc_cluster_req, &req);
            __io_lock();
            pi_cl_send_task_to_fc(&(req.cb));
            cl_wait_task(&(req.done));
            __io_unlock();
            *index = 0;
            memset(buffer, 0, (uint32_t) PRINTF_BUFFER_SIZE);
        }
    }
    else
    #endif  /* FEATURE_CLUSTER */
    {
        g_printf_semihost_buffer[g_printf_semihost_index] = c;
        g_printf_semihost_index++;
        if ((g_printf_semihost_index == ((uint32_t) PRINTF_BUFFER_SIZE - 1)) ||
            (c == '\n'))
        {
            __io_lock();
            __semihost_buffer_write_exec(g_printf_semihost_buffer,
                                         g_printf_semihost_index);
            __io_unlock();
            g_printf_semihost_index = 0;
            memset(g_printf_semihost_buffer, 0, (uint32_t) PRINTF_BUFFER_SIZE);
        }
    }
}

static void __semihost_printf_flush(char c)
{
    if ((g_printf_semihost_index > 0) &&
        (g_printf_semihost_index < (uint32_t) PRINTF_BUFFER_SIZE))
    {
        g_printf_semihost_buffer[g_printf_semihost_index++] = c;
        __semihost_buffer_write_exec(g_printf_semihost_buffer,
                                     g_printf_semihost_index);
    }
}

#else  /* PRINTF_SEMIHOST */
#if defined(PRINTF_RTL)

static void __stdout_putc(char c)
{
    #if defined(FEATURE_CLUSTER)
    if (!__native_is_fc())
    {
        CLUSTER_STDOUT->PUTC[__core_ID() << 1] = c;
    }
    else
    #endif  /* FEATURE_CLUSTER */
    {
        FC_STDOUT->PUTC[__core_ID() << 1] = c;
    }
}
#endif  /* PRINTF_RTL */
#endif  /* PRINTF_UART */

static uint32_t __is_irq_mode()
{
    uint32_t is_irq = 0;
    if (__get_CPRIV())
    {
        if (__get_MCAUSE() & MCAUSE_IRQ_Msk)
        {
            is_irq = 1;
        }
    }
    else
    {
        if (__get_UCAUSE() & MCAUSE_IRQ_Msk)
        {
            is_irq = 1;
        }
    }
    return is_irq;
}

__attribute__((noinline)) void __io_lock()
{
    if (pi_cluster_is_on())
    {
        while (__tas_lock() == 0xFFFFFFFF)
        {
            #if defined(FEATURE_CLUSTER)
            if (!__native_is_fc())
            {
                hal_eu_evt_mask_wait_and_clr(1 << (uint32_t) PRINTF_LOCK_IRQN);
            }
            else
            #endif  /* FEATURE_CLUSTER */
            {
                if (g_freertos_scheduler_started)
                {
                    if (!__is_irq_mode())
                    {
                        //pi_yield();
                    }
                }
            }
        }
    }
    else
    {
        if (g_freertos_scheduler_started)
        {
            if (!__is_irq_mode())
            {
                BaseType_t ret = 0;
                xSemaphoreTakeFromISR(g_printf_mutex, &ret);
            }
        }
    }
}

__attribute__((noinline)) void __io_unlock()
{
    if (pi_cluster_is_on())
    {
        __tas_unlock();

        #if defined(FEATURE_CLUSTER)
        hal_eu_cluster_evt_trig_set((uint32_t) PRINTF_LOCK_IRQN, 0);
        #endif  /* FEATURE_CLUSTER */
    }
    else
    {
        if (g_freertos_scheduler_started)
        {
            if (!__is_irq_mode())
            {
                BaseType_t ret = 0;
                xSemaphoreGiveFromISR(g_printf_mutex, &ret);
            }
        }
    }
}

static void tfp_putc(void *data, char c)
{
    #if !defined(__DISABLE_PRINTF__)
    #if defined(PRINTF_UART)
    if (g_freertos_scheduler_started)
    {
        __uart_putc(c);
    }
    #elif defined(PRINTF_SEMIHOST)
    __semihost_putc(c);
    #elif defined(PRINTF_RTL)  /* PRINTF_SEMIHOST */
    /* This is for core internal printf in Simulation */
    __stdout_putc(c);
    #endif  /* PRINTF_RTL */
    #endif /* __DISABLE_PRINTF__ */
}

void _putchar(char character)
{
    tfp_putc(NULL, character);
}

int puts(const char *s)
{
    char c;
    //__io_lock();
    do
    {
        c = *s;
        if (c == 0)
        {
            tfp_putc(NULL, '\n');
            break;
        }
        tfp_putc(NULL, c);
        s++;
    } while (1);
    //__io_unlock();
    return 0;
}

void system_exit_printf_flush()
{
    #if !defined(__DISABLE_PRINTF__)
    char end_line = '\n';
    #if defined(PRINTF_UART)
    __uart_printf_flush(end_line);
    #elif defined(PRINTF_SEMIHOST)
    __semihost_printf_flush(end_line);
    #elif defined(PRINTF_RTL)
    __stdout_putc(end_line);
    #endif  /* PRINTF_RTL */
    #endif  /* __DISABLE_PRINTF__ */
}
