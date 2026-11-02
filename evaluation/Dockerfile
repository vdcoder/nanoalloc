# nanoalloc evaluation environment.
#
# Build once (network needed only here), then evaluate every candidate with
# the same image and no network:
#   docker build -t nanoalloc-evaluation:1.0 .
#   docker run --rm --network none \
#     -v /path/to/buddy-baseline:/inputs/baseline:ro \
#     -v /path/to/candidate:/inputs/candidate:ro \
#     -v /path/to/results:/results \
#     nanoalloc-evaluation:1.0 --baseline /inputs/baseline \
#     --candidate /inputs/candidate --out /results [--threshold PERCENT]
#
# BASE_IMAGE can point to a mirror of the same image, e.g.
#   --build-arg BASE_IMAGE=mirror.gcr.io/library/ubuntu:24.04
ARG BASE_IMAGE=ubuntu:24.04
FROM ${BASE_IMAGE}

RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      g++ cmake ninja-build python3 \
 && rm -rf /var/lib/apt/lists/*

COPY . /opt/nanoalloc-evaluation
RUN chmod -R a+rX /opt/nanoalloc-evaluation

# Recorded in every report, to identify the environment.
ARG IMAGE_LABEL=nanoalloc-evaluation:1.0
ENV NANOEVAL_IMAGE=${IMAGE_LABEL} \
    CXX=g++
WORKDIR /results
ENTRYPOINT ["python3", "/opt/nanoalloc-evaluation/evaluate.py"]
CMD ["--help"]
