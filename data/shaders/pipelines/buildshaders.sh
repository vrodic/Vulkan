#!/bin/bash

find -type f -name "*.vert" | \
	while read f; do $VULKAN_SDK/bin/glslangValidator -V ${f} -o "${f}.spv"; done

find -type f -name "*.frag" | \
	while read f; do $VULKAN_SDK/bin/glslangValidator -V ${f} -o "${f}.spv"; done