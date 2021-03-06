#!/usr/bin/env bash
# Copyright (c) the JPEG XL Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"$(dirname "$0")/run_all_sdr_metrics.sh" "$@" | sed -n '/```/q;p' > sdr_results.csv
mkdir -p sdr_plots/
rm -rf sdr_plots/*
python3 "$(dirname "$0")/plots.py" sdr_results.csv sdr_plots
