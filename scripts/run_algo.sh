#!/bin/bash
echo "run_algo.sh...."
set -e

HDFS="" # the prefix of hdfs path
WNUM=4 # number of processes
WCORES=4 # number of threads

ALGO="" # algorithm name

IS_DIRECTED=true

# louvain
OUTER_ITERATION=10
INNER_ITERATION=10
TOL=0.5

# kcore
#ITERATIONS=10
TYPE="vertex"
K=5

# lpa
ITERATIONS=10

# hanp
# ITERATIONS=10
PREFERENCE=1.0
HOP_ATT=0.1

# pagerank
# ITERATIONS=10
DAMPING=0.85

while getopts f:n:c:a:d:o:i:t:e:k:r:p:h:m: opt;
do
    echo "getopts....."
    case $opt in
        f)
            echo $OPTARG
            HDFS=$OPTARG
            ;;
        n)
            echo $OPTARG
            WNUM=$OPTARG
            ;;
        c)
            echo $OPTARG
            WCORES=$OPTARG
            ;;
        a)
            echo $OPTARG
            ALGO=$OPTARG
            ;;
        d)  
            echo $OPTARG
            IS_DIRECTED=$OPTARG
            ;;
        o)
            echo $OPTARG
            OUTER_ITERATION=$OPTARG
            ;;
        i)
            echo $OPTARG
            INNER_ITERATION=$OPTARG
            ;;
        t)
            echo $OPTARG
            TOL=$OPTARG
            ;;
        e)
            echo $OPTARG
            TYPE=$OPTARG
            ;;
        k)  
            echo $OPTARG
            K=$OPTARG
            ;;
        r)  
            echo $OPTARG
            ITERATIONS=$OPTARG
            ;;
        p)
            echo $OPTARG
            PREFERENCE=$OPTARG
            ;;
        h)
            echo $OPTARG
            HOP_ATT=$OPTARG
            ;;
        m)
            echo $OPTARG
            DAMPING=$OPTARG
            ;;
        ?)
            echo "Invalid option: $OPTARG"
            exit 1
            ;;
    esac
done

PROJECT="/home/vesoft-cm/jie/plato/"
MAIN="./bazel-bin/example/${ALGO}" # process name
OUTPUT_NAME="invalid_name"

ALPHA=-1
PART_BY_IN=false

export MPIRUN_CMD=${MPIRUN_CMD:='/home/vesoft-cm/jie/plato/3rd/mpich-3.2.1/bin/mpiexec.hydra'}
export JAVA_HOME=${APP_JAVA_HOME:='/usr/local/java/jdk1.8.0_271'}
export HADOOP_HOME=${APP_HADOOP_HOME:='/usr/local/hadoop-2.10.0'}
export HADOOP_CONF_DIR="${HADOOP_HOME}/etc/hadoop"

case ${ALGO} in
   "fast_unfolding_simple")
        OUTPUT_NAME="louvain"
        PARAMS+=" --outer_iteration ${OUTER_ITERATION} --inner_iteration ${INNER_ITERATION}"
        ;;
   "kcore_simple")
        OUTPUT_NAME="kcore"
        PARAMS+=" --type ${TYPE} --kmax ${K}"
        ;;
   "lpa")
        OUTPUT_NAME="lpa"
        PARAMS+=" --iterations ${ITERATIONS}"
        ;;
   "hanp")
        OUTPUT_NAME="hanp"
        PARAMS+=" --iterations ${ITERATIONS} --preference ${PREFERENCE} --hop_att ${HOP_ATT}"
        ;;
   "pagerank")
        OUTPUT_NAME="pagerank"
        PARAMS+=" --iterations ${ITERATIONS} --damping ${DAMPING}"
        ;;
    *)
        echo "No algorithm: ${ALGO}"
        exit 1
        ;;
esac

INPUT_DIR="/_plato/data/"
OUTPUT_DIR="/_plato/algo/${OUTPUT_NAME}"

INPUT="${HDFS}/${INPUT_DIR}"
OUTPUT="${HDFS}/${OUTPUT_DIR}"
NOT_ADD_REVERSED_EDGE=${NOT_ADD_REVERSED_EDGE:=true}  # let plato auto add reversed edge or not

PARAMS+=" --threads ${WCORES}"
PARAMS+=" --input ${INPUT} --output ${OUTPUT}"
PARAMS+=" --is_directed=${IS_DIRECTED}"

# env for JAVA && HADOOP
export LD_LIBRARY_PATH=${JAVA_HOME}/jre/lib/amd64/server:${LD_LIBRARY_PATH}

# env for hadoop
export CLASSPATH=${HADOOP_HOME}/etc/hadoop:`find ${HADOOP_HOME}/share/hadoop/ | awk '{path=path":"$0}END{print path}'`
export LD_LIBRARY_PATH="${HADOOP_HOME}/lib/native":${LD_LIBRARY_PATH}

hdfs dfs -rm -r -f ${OUTPUT_DIR}

echo "Start running plato..."
echo "${MPIRUN_CMD} -n ${WNUM} -f ${PROJECT}/scripts/cluster ${PROJECT}/${MAIN} ${PARAMS}"
chmod 777 ${PROJECT}/${MAIN}

# create log dir if it doesn't exist
LOG_DIR=/tmp/graphcompute/${OUTPUT_NAME}
if [ ! -d ${LOG_DIR} ]; then
    mkdir -p ${LOG_DIR}
fi

${MPIRUN_CMD} -n ${WNUM} -f ${PROJECT}/scripts/cluster ${PROJECT}/${MAIN} ${PARAMS} --log_dir=${LOG_DIR}
if [ $? -ne 0 ]; then
    echo $?
    exit $?
fi

# process output gzip files
gzip_files=($(hadoop fs -ls ${OUTPUT_DIR} | awk '{if (NR>1){print $NF}}'))
echo "数组元素个数为: ${#gzip_files[*]}"
echo "数组元素个数为: ${#gzip_files[@]}"

for gzip_file in ${gzip_files[@]}
do
echo $gzip_file
echo ${gzip_file:0:(-3)} 
csv_file=${gzip_file:0:(-3)}
hadoop fs -cat $gzip_file | gzip -d | sed "1i  _algoId,${OUTPUT_NAME}" | hadoop fs -put - $csv_file
hadoop fs -rm $gzip_file
done

exit $?
