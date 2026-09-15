# This script is for calculating
# port statisctics from file.
#
# Record packet statistics in a file
# with following command:
# bessctl monitor port > port_data

set -o errexit
set -o pipefail
set -o nounset

# Take port data file as argument
port_data_file=$1

pkt_drop_in=$(cat ${port_data_file} | grep pmdportPMDPort | awk 'BEGIN{l=0}{l+=$4}END{print l}')
pkt_pps_in=$(cat ${port_data_file} | grep pmdportPMDPort | awk 'BEGIN{l=0}{l+=$3}END{print l/NR}')

echo "IN: Tput(avg) -> ${pkt_pps_in} Mpps    Dropped -> ${pkt_drop_in} packets"
