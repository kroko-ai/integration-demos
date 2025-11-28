#!/bin/bash

# Change API key before starting Asterisk
if [ -n "$API_KEY" ]; then
    sed -i "s/apiKey-from-kroko.ai/${API_KEY}/g" /etc/asterisk/res_speech_kroko.conf
fi

# starting Asterisk
exec asterisk -f
