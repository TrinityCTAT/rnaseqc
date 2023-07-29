#!/bin/bash


singularity build rnaseqc.simg docker://trinityctat/rnaseqc:latest

singularity exec -e  rnaseqc.simg  rnaseqc -h

