#!/usr/bin/perl

use v5.12;
use File::Basename;

my ($job_id, $dir) = @ARGV or die;

mkdir $dir or die $!;

open my $fh, "knit-cat-file -p $job_id |" or die $!;
while (<$fh>) {
    chomp;
    my ($digest, $path) = split(/\t/, $_, 2);
    die unless $path;
    system('mkdir', '-p', dirname "$dir/in/$path") == 0 or die $?;
    open STDOUT, '>', "$dir/in/$path" or die $!;
    system('knit-cat-file', 'resource', $digest) == 0 or die $?;
}
close $fh or die $!;
