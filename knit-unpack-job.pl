#!/usr/bin/perl

use v5.12;
use File::Basename;

my ($job_id, $dir) = @ARGV or die;

mkdir $dir or die $!;

open my $fh, "knit-cat-file job $job_id |" or die $!;
local $/ = "\0";
while (<$fh>) {
    chomp(my $path = $_);
    my $digest;
    read $fh, $digest, 32 or die $!;
    system('mkdir', '-p', dirname "$dir/in/$path") == 0 or die $?;
    open STDOUT, '>', "$dir/in/$path" or die $!;
    system('knit-cat-file', 'resource', unpack('H*', $digest)) == 0 or die $?;
}
close $fh or die $!;
