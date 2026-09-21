"""Adversarial float32 check of sample rejection against the existing displacement."""
import itertools
import random
import struct


def f(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def add(a, b):
    return f(f(a) + f(b))


def mul(a, b):
    return f(f(a) * f(b))


def lerp(a, b, t):
    return add(a, mul(f(b - a), t))


def check(t, wobble, variety, noise, mid):
    # Evaluate the original displacement from saturated texture channels.
    wn = lerp(f(lerp(noise[0], noise[1], .5) - .5),
              f(mul(lerp(noise[2], noise[3], .5), .3) - f(.15)), t)
    wm = lerp(f(mid[0] - .5), f(mul(mid[1], .3) - f(.15)), t)
    displacement = add(mul(add(mul(wn, .6), mul(wm, .4)), wobble), mul(wm, variety))
    bound = lerp(.5, .15, t)
    margin = mul(1e-5, max(1, add(wobble, variety)))
    first_threshold = add(mul(bound, add(wobble, variety)), margin)
    assert f(first_threshold - displacement) >= 0, 'first gate rejected material'

    # Probe just outside the proposed mid-gate boundary, including cancellation.
    mid_limit = add(mul(mul(.6, bound), wobble), margin)
    mid_push = add(mul(mul(.4, wm), wobble), mul(wm, variety))
    for epsilon in (0, 1e-7, 1e-5):
        sdf = add(add(mid_limit, mid_push), epsilon)
        after_mid = f(f(sdf - mul(mul(.4, wm), wobble)) - mul(wm, variety))
        if after_mid >= mid_limit:
            assert f(sdf - displacement) >= 0, 'mid gate rejected material'


count = 0
for t, wobble, variety in itertools.product((0, .001, .25, .5, .999, 1), (0, .01, .6, 3, 60), (0, .01, .5, 1.5)):
    for channels in itertools.product((0., 1.), repeat=6):
        check(f(t), f(wobble), f(variety), channels[:4], channels[4:])
        count += 1
rng = random.Random(614)
for _ in range(30000):
    channels = [f(rng.random()) for _ in range(6)]
    check(f(rng.random()), f(10 ** rng.uniform(-5, 2)), f(rng.random() * 1.5), channels[:4], channels[4:])
    count += 1
print(f'{count} float32 displacement cases passed; no rejected sample could contain material.')
