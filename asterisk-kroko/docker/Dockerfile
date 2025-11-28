# Asterisk server Docker container
# author: Dimitar Kokov
# version: 0.7
# date: 2025-11-28

########## BUILD STAGE ##########

FROM debian:bookworm-slim AS build

ARG ASTERISK_VERSION=23

#INSTALL DEPENDENCIES
RUN apt-get update && \
apt-get install -y wget gcc make git autoconf automake sudo apt-utils tzdata net-tools \
libedit-dev build-essential libssl-dev libncurses5-dev libnewt-dev libxml2-dev libsqlite3-dev uuid-dev libjansson-dev libsrtp2-dev \
libspeex-dev libspeexdsp-dev libcurl4 'libtool-bin|libtool' pkg-config \
&& rm -rf /var/lib/apt/lists/*

RUN git clone --branch releases/${ASTERISK_VERSION} https://github.com/asterisk/asterisk.git /usr/src/asterisk

COPY . /usr/src/asterisk-kroko/

# asterisk
RUN cd /usr/src/asterisk/ && ./configure  && make && make install && ldconfig

# copy Kroko setup to the Asterisk
COPY ./docker/conf/* /etc/asterisk/

# Asterisk OPUS module
RUN cd /usr/src/ && wget http://downloads.digium.com/pub/telephony/codec_opus/asterisk-${ASTERISK_VERSION}.0/x86-64/codec_opus-${ASTERISK_VERSION}.0-current-x86_64.tar.gz
RUN cd /usr/src/ && tar -zxf codec_opus-${ASTERISK_VERSION}.0-current-x86_64.tar.gz
RUN cd /usr/src/codec_opus-${ASTERISK_VERSION}.0_* && cp codec_opus.so /usr/lib/asterisk/modules/ && cp format_ogg_opus.so /usr/lib/asterisk/modules/ && \
cp codec_opus_config-en_US.xml /var/lib/asterisk/documentation/

# asterisk-kroko modules
RUN cd /usr/src/asterisk-kroko/ && ./bootstrap
RUN cd /usr/src/asterisk-kroko/ && ./configure --with-asterisk=/usr/src/asterisk/ --prefix=/usr/ 
RUN cd /usr/src/asterisk-kroko/ && make && make install

RUN cp -v /usr/src/asterisk-kroko/res-speech-kroko/res_speech_kroko.conf /etc/asterisk/

# Cleanup the image
RUN apt-get clean


########## RUNTIME STAGE ##########
FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive
ENV NAME kroko-ast

# Only runtime libs, no gcc, no make
RUN apt-get update && apt-get install -y --no-install-recommends \
    libedit2 libssl3 libncurses5 libxml2 libsqlite3-0 uuid-runtime \
    libjansson4 libsrtp2-1 libspeex1 libspeexdsp1 libcurl4 \
    && rm -rf /var/lib/apt/lists/*

# Copy only WHAT IS NEEDED
COPY --from=build /usr/sbin/asterisk /usr/sbin/
COPY --from=build /etc/asterisk/ /etc/asterisk/
COPY --from=build /usr/lib/asterisk /usr/lib/asterisk
COPY --from=build /usr/lib/libasterisk* /usr/lib/
COPY --from=build /var/lib/asterisk/documentation /var/lib/asterisk/documentation


RUN mkdir -p /var/lib/asterisk

RUN mkdir -p /var/lib/asterisk/sounds/en
COPY ../viki_test.gsm /var/lib/asterisk/sounds/en/

COPY ./docker/asterisk-kroko.sh /asterisk-kroko.sh
RUN chmod +x /asterisk-kroko.sh
ENTRYPOINT ["/asterisk-kroko.sh"]