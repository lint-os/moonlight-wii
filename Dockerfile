FROM devkitpro/devkitppc:20260503

# devkitPPC mbedtls 3.6.4 (wii-curl release), verified by sha256
RUN curl -fL -o /tmp/wii-mbedtls.pkg.tar.gz \
        'https://github.com/AndrewPiroli/wii-curl/releases/download/c8.16.0%2Bm3.6.4/wii-mbedtls-3.6.4-1-any.pkg.tar.gz' \
    && echo '0b0d3eb9aa93fd26f9caf2a52f8236354861daf4c558b95e0a3f67bc66c655de  /tmp/wii-mbedtls.pkg.tar.gz' \
        | sha256sum -c - \
    && dkp-pacman -U --noconfirm /tmp/wii-mbedtls.pkg.tar.gz \
    && rm /tmp/wii-mbedtls.pkg.tar.gz

WORKDIR /project
