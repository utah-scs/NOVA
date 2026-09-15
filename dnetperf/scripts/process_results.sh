#!/bin/bash

root_dir="$1"
output_file="$2"
batch_dirs=$(basename -a $(ls -d ${root_dir}/*) | sort -n -t _ -k 2)

echo "tput,avg lat,p50 lat,p99 lat" > $output_file
echo "0,0" >> $output_file
for batch_dir in ${batch_dirs}; do
	batch_dir=${root_dir}/${batch_dir}
	data_dirs=$(basename -a $(ls -d ${batch_dir}/*) | sort -n -t _ -k 2)
	
	for d in ${data_dirs}; do
		dir="${batch_dir}/$d"
		echo $dir
		thput=$(cat ${dir}/client_* | grep reqs | awk 'BEGIN{sum=0}{sum += $3}END{print sum/1000000}')
		lat_avg=$(cat ${dir}/client_* | grep mean | awk 'BEGIN{sum=0}{sum += $4}END{print sum/NR}')
		lat_p50=$(cat ${dir}/client_* | grep median | awk 'BEGIN{sum=0}{sum += $4}END{print sum/NR}')
		lat_p99=$(cat ${dir}/client_* | grep 99th | awk 'BEGIN{sum=0}{sum += $4}END{print sum/NR}')
		echo "$thput,$lat_avg,$lat_p50,$lat_p99" >> $output_file
	done
done
