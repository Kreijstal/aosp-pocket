# SPDX-License-Identifier: GPL-3.0-only
# Source after linkerconfig/derive_classpath; never substitute host classpaths.
test -s /tmp/pocket-runtime-classpath
while read -r directive name value; do
    test "$directive" = export
    case "$name" in
        BOOTCLASSPATH|DEX2OATBOOTCLASSPATH|SYSTEMSERVERCLASSPATH) export "$name=$value" ;;
        *) return 1 ;;
    esac
done < /tmp/pocket-runtime-classpath
export BOOTCLASSPATH="$BOOTCLASSPATH:/system/framework/pocket-zygote.jar"
export ANDROID_ART_ROOT=/apex/com.android.art
export ANDROID_I18N_ROOT=/apex/com.android.i18n
export ANDROID_TZDATA_ROOT=/apex/com.android.tzdata
unset LD_LIBRARY_PATH
