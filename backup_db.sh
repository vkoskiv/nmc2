#!/bin/bash

cwd=`dirname $0`

# Delete backups older than a week
find $cwd/backups -mmin +10080 -type f -delete

# Invoke the server to save a new backup
# FIXME: Would be ideal to not send this signal to every running nmc2 instance
kill -SIGUSR2 $(pidof nmc2)
sleep 2

# Compress 'em
for file in $(find $cwd/backups -name '*.db'); do
	gzip $file
done
