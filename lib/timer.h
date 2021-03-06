/*
 * Copyright (c) 2011 Nicira Networks.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef TIMER_H
#define TIMER_H 1

#include <stdbool.h>

#include "timeval.h"

struct timer {
    long long int t;
};

long long int timer_msecs_until_expired(const struct timer *);
long long int timer_enabled_at(const struct timer *, long long int duration);
void timer_wait(const struct timer *);

/* Causes 'timer' to expire when 'duration' milliseconds have passed.
 *
 * May be used to initialize 'timer'. */
static inline void
timer_set_duration(struct timer *timer, long long int duration)
{
    timer->t = time_msec() + duration;
}

/* Causes 'timer' never to expire.
 *
 * May be used to initialize 'timer'. */
static inline void
timer_set_infinite(struct timer *timer)
{
    timer->t = LLONG_MAX;
}

/* Causes 'timer' to expire immediately.
 *
 * May be used to initialize 'timer'. */
static inline void
timer_set_expired(struct timer *timer)
{
    timer->t = LLONG_MIN;
}

/* True if 'timer' had (or will have) expired at 'time'. */
static inline bool
timer_expired_at(const struct timer *timer, long long int time)
{
    return time >= timer->t;
}

/* True if 'timer' has expired. */
static inline bool
timer_expired(const struct timer *timer)
{
    return timer_expired_at(timer, time_msec());
}

/* Returns ture if 'timer' will never expire. */
static inline bool
timer_is_infinite(const struct timer *timer)
{
    return timer->t == LLONG_MAX;
}

#endif /* timer.h */
