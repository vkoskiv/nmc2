#!/bin/bash

cwd=`dirname $0`

# Delete backups older than a week
find $cwd/backups -mmin +10080 -type f -delete

# Invoke the server to save a new backup
kill -SIGUSR2 $(<"$cwd/nmc2.pid") || exit 1
sleep 2

# Compress 'em
for file in $(find $cwd/backups -name '*.db'); do
	gzip $file
done
