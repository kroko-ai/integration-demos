# FreeSWITCH server Docker container
# author: Dimitar Kokov
# version: 0.5
# date: 2025-10-15

# docker image build -t kroko-fs .
#
# https://github.com/signalwire/freeswitch/blob/master/docker/examples/Debian11/Dockerfile
# https://github.com/drachtio/docker-drachtio-freeswitch-base/blob/main/Dockerfile
#

FROM debian:bullseye-slim
#FROM debian:12.2

ENV DEBIAN_FRONTEND noninteractive
ENV NAME kroko-fs
ENV apiKey "<apiKey from kroko.ai>"


#INSTALL DEPENDENCIES
RUN apt-get update && \
apt-get install -y wget gcc make git autoconf automake sudo apt-utils tzdata net-tools \
libtiff-dev cmake htop mc vim 

RUN apt-get -yq install \
build-essential cmake automake autoconf 'libtool-bin|libtool' pkg-config \
libssl-dev zlib1g-dev libdb-dev unixodbc-dev libncurses5-dev libexpat1-dev libgdbm-dev bison erlang-dev libtpl-dev libtiff5-dev uuid-dev \
libpcre3-dev libedit-dev libsqlite3-dev libcurl4-openssl-dev nasm libogg-dev libspeex-dev libspeexdsp-dev \
libldns-dev python3-dev libavformat-dev libswscale-dev libavresample-dev \
liblua5.2-dev libopus-dev libpq-dev libsndfile1-dev libflac-dev libogg-dev libvorbis-dev \
libshout3-dev libmpg123-dev libmp3lame-dev

#RUN git clone https://github.com/signalwire/freeswitch /usr/src/freeswitch
RUN git clone --branch v1.10.12 --depth 1 https://github.com/signalwire/freeswitch /usr/src/freeswitch
RUN git clone https://github.com/freeswitch/sofia-sip /usr/src/libs/sofia-sip
RUN git clone https://github.com/freeswitch/spandsp /usr/src/libs/spandsp
RUN git clone https://github.com/warmcat/libwebsockets.git -b v4.3.2 /usr/src/libs/libwebsockets

RUN cd /usr/src/libs/sofia-sip && \
./bootstrap.sh && ./configure CFLAGS="-g -ggdb" --with-pic --with-glib=no --without-doxygen --disable-stun --prefix=/usr && \
make -j`nproc --all` && make install

RUN cd /usr/src/libs/spandsp && ./bootstrap.sh && ./configure CFLAGS="-g -ggdb" --with-pic --prefix=/usr && make -j`nproc --all` && make install

# libwebsockets , C lib
RUN cd /usr/src/libs/libwebsockets && \
mkdir -p build && cd build && \
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo && \
make && make install && ldconfig

WORKDIR /kroko-fs
COPY ./docker/conf/ ./conf/
COPY ./docker/modules.conf /usr/src/freeswitch
COPY ./mod_kroko_transcribe/ /usr/src/freeswitch/src/mod/asr_tts/mod_kroko_transcribe/
COPY ./viki_test.wav /kroko-fs/

RUN cd /usr/src/freeswitch && \
sed -i '/src\/mod\/asr_tts\/mod_tts_commandline\/Makefile/a\\t\tsrc\/mod\/asr_tts\/mod_kroko_transcribe\/Makefile' configure.ac

# FreeSWITCH - compile and install
RUN cd /usr/src/freeswitch && ./bootstrap.sh -j
RUN cd /usr/src/freeswitch && ./configure
RUN cd /usr/src/freeswitch && make -j`nproc` && make install

RUN cd /usr/src/freeswitch && make sounds-install && make moh-install

# copy Kroko setup to the FreeSWITCH
RUN rm -fvR /usr/local/freeswitch/conf/
RUN cp -fvR ./conf/ /usr/local/freeswitch/conf/
RUN cp -v viki_test.wav /usr/local/freeswitch/sounds/en/us/callie/
RUN sed -i "s/<apiKey>/${apiKey}/g" /usr/local/freeswitch/conf/dialplan/qa_team.xml

# Cleanup the image
RUN apt-get clean

CMD ["/usr/local/freeswitch/bin/freeswitch"]

