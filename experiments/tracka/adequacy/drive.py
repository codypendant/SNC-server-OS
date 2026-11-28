import json
import sys

from tracka.run import RUNS, Run


def main():
    arm_dir, name, point, seed, scenario = sys.argv[1:6]
    r = Run(arm_dir, point, int(seed), scenario)
    r.arm = name
    tail = f"{point}-{scenario}-{seed}" if point == "none" else f"{point}-{seed}"
    r.R = RUNS / f"{name}-{tail}"
    print(json.dumps(r.execute()), flush=True)


if __name__ == "__main__":
    main()
