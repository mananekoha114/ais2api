// Copyright (c) 2026 ais2api contributors
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
import { afterEach, expect, it } from 'vitest'
import { dollars, usageBounds } from './usage'
const originalTimezone = process.env.TZ
afterEach(() => { if (originalTimezone === undefined) delete process.env.TZ; else process.env.TZ = originalTimezone })

it('uses local calendar days across both DST transitions', () => {
  process.env.TZ = 'America/New_York'
  const spring = usageBounds('today', new Date(2026, 2, 8, 12))
  const fall = usageBounds('today', new Date(2026, 10, 1, 12))
  expect(spring.to! - spring.from!).toBe(23 * 3600000)
  expect(fall.to! - fall.from!).toBe(25 * 3600000)
  const week = usageBounds('week', new Date(2026, 2, 9, 12))
  expect(new Date(week.from!).getDate()).toBe(3)
  expect(new Date(week.to!).getDate()).toBe(10)
})
it('recomputes today after midnight and validates custom inclusive dates', () => {
  const before = usageBounds('today', new Date(2026, 9, 29, 23, 59))
  const after = usageBounds('today', new Date(2026, 9, 30, 0, 1))
  expect(before.to).toBe(after.from)
  expect(usageBounds('yesterday', new Date(2026, 9, 30, 12))).toEqual(before)
  expect(usageBounds('custom', new Date(), '2026-10-29', '2026-10-29')).toEqual(before)
  expect(() => usageBounds('custom', new Date(), '2026-02-30', '2026-03-01')).toThrow('日期无效')
  expect(() => usageBounds('custom', new Date(), '2026-03-02', '2026-03-01')).toThrow('结束日期')
  expect(usageBounds('all')).toEqual({})
})
it('keeps unknown amounts distinct from zero and displays nanodollar precision', () => {
  expect(dollars(null)).toBe('未完整计价')
  expect(dollars(0)).toBe('$0')
  expect(dollars(1)).toBe('$0.000000001')
  expect(dollars(1250000000)).toBe('$1.25')
})
