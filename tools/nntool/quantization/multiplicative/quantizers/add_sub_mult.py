# Copyright (C) 2020  GreenWaves Technologies, SAS

# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as
# published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.

# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

from copy import deepcopy

import numpy as np
from graph.types import MatrixAddParameters, MatrixSubParameters
from quantization.new_qrec import QRec
from quantization.qtype import QType
from quantization.unified_quantization_handler import (in_qs_constraint,
                                                       out_qs_constraint,
                                                       params_type)

from ..mult_quantization_handler import MultQuantizionHandler


@params_type(MatrixAddParameters, MatrixSubParameters)
@in_qs_constraint({'dtype': np.int8},
                  {'dtype': np.int8})
@out_qs_constraint({'dtype': np.int8})
class AddSubMult(MultQuantizionHandler):
    @classmethod
    def _quantize(cls, params, in_qs, stats, **kwargs):
        force_out_qs, out_dtype = cls.get_mult_opts(**kwargs)
        force_out_q = force_out_qs and force_out_qs[0]
        # NOTE: The autotiler kernel scales and clips after the operation and before the
        # activation so there is no change if this is in a fusion or not
        scaled_idx = params.force_quantized_index if isinstance(
            params, MatrixAddParameters) else None
        in_qs = cls.force_symmetric_and_dtype(in_qs)
        if in_qs is None:
            return None

        if force_out_q:
            o_q = deepcopy(force_out_q)
            if o_q.is_asymmetric:
                return None
        else:
            cls.check_valid_ranges(params, stats, idx=0, dirs='out')
            o_q = QType.from_min_max_sq(stats['range_out'][0]['min'],
                                        stats['range_out'][0]['max'],
                                        dtype=out_dtype)
        o_q.set_forced(flags=['dtype', 'zero_point'])
        in_qs = [in_q.set_forced(flags=['dtype', 'zero_point']) for in_q in in_qs]
        return QRec.scaled(in_qs=in_qs, out_qs=[o_q], scaled_idx=scaled_idx)
