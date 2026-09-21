"""Summarize complete rendered-frame GPU stage samples from a Remix log."""
import argparse
import math
import re
import statistics
from collections import defaultdict
from pathlib import Path


def read_samples(text):
    frames = defaultdict(dict)
    pattern = r"\[GPU stages\] frame=(\d+) stage=(\w+) ms=([0-9.eE+\-]+)"
    for frame, stage, value in re.findall(pattern, text):
        value = float(value)
        if math.isfinite(value) and value >= 0:
            frames[int(frame)][stage] = value
    return [(frame, stages) for frame, stages in sorted(frames.items())
            if 'MeasuredSequence' in stages and 'GBuffer' in stages
            and 'OutputAndFrameGeneration' in stages]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--last', type=int, default=20)
    parser.add_argument('--min-frame', type=int, default=0)
    args = parser.parse_args()
    if args.last < 1:
        parser.error('--last must be positive')
    frames = [(f, s) for f, s in read_samples(args.log.read_text(errors='replace')) if f >= args.min_frame][-args.last:]
    if not frames:
        raise SystemExit('No complete rendered-frame samples yet.')
    values = defaultdict(list)
    for _, stages in frames:
        for stage, value in stages.items():
            values[stage].append(value)
    print(f'{len(frames)} sampled frames: {frames[0][0]}..{frames[-1][0]}')
    print('Stage                                  median ms     min ms     max ms    samples')
    for stage, samples in sorted(values.items(), key=lambda pair: statistics.median(pair[1]), reverse=True):
        print(f'{stage:38} {statistics.median(samples):9.3f} {min(samples):10.3f} {max(samples):10.3f} {len(samples):10}')
    print('MeasuredSequence overlaps all listed stages; do not add it to them. GPU intervals exclude game CPU and presentation work.')


if __name__ == '__main__':
    main()
