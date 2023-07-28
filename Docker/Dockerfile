# Dockerfile for RNASeQC
FROM ubuntu:20.04
MAINTAINER Aaron Graubert

RUN apt-get update && apt-get install -y software-properties-common && \
    apt-get update && apt-get install -y \
        build-essential \
        cmake \
        git \
        python3 \
        python3-pip \
        libboost-filesystem-dev \
        libboost-regex-dev \
        libboost-system-dev \
        libbz2-dev \
        libcurl3-dev \
        liblzma-dev \
        libpthread-stubs0-dev \
        wget \
        zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*




WORKDIR /usr/local/src
RUN git clone --recursive https://github.com/TrinityCTAT/rnaseqc.git && \
      cd rnaseqc && \
      git checkout mm2_integration && \
      make && \
      ln -sf /usr/local/src/rnaseqc/rnaseqc /usr/local/bin/rnaseqc

