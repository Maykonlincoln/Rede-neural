# Owner(s): ["oncall: distributed"]

import sys
import torch
import torch.distributed as dist
import torch.distributed._functional_collectives as ft_c
import torch.distributed.distributed_c10d as c10d
import torch.distributed._tensor as dt

if not dist.is_available():
    print("Distributed not available, skipping tests", file=sys.stderr)
    sys.exit(0)

from torch.testing._internal.common_distributed import (
    MultiThreadedTestCase,
)
from torch.testing._internal.common_utils import (
    run_tests,
)

def new_subgroups(group_size: int, pg_tag=None):
    world_size = dist.get_world_size()
    subgroups = []
    cur_subgroup = None

    for subgroup_id in range(world_size // group_size):
        start_rank = subgroup_id * group_size
        end_rank = start_rank + group_size
        ranks_in_subgroup = list(range(start_rank, end_rank))
        subgroup = dist.new_group(
            ranks=ranks_in_subgroup,
            pg_tag=pg_tag,
        )
        subgroups.append(subgroup)

        rank = dist.get_rank()
        if rank in ranks_in_subgroup:
            cur_subgroup = subgroup

    return cur_subgroup, subgroups


class TestExpand(MultiThreadedTestCase):
    @property
    def world_size(self):
        return 4

    def setUp(self):
        super().setUp()
        self._spawn_threads()

    def test_expand_1d_rank_list(self):
        tag, rankset, stride = ft_c._expand_group([0, 1, 2, 3])
        self.assertEqual("", tag)
        self.assertEqual([0, 1, 2, 3], rankset)
        self.assertEqual(4, stride)

        tag, rankset, stride = ft_c._expand_group([0, 1, 2, 3], "bla")
        self.assertEqual("bla", tag)

    def test_expand_2d_rank_list(self):
        tag, rankset, stride = ft_c._expand_group([[0, 1], [2, 3]])
        self.assertEqual("", tag)
        self.assertEqual([0, 1, 2, 3], rankset)
        self.assertEqual(2, stride)

        tag, rankset, stride = ft_c._expand_group([[0, 1], [2, 3]], "blu")
        self.assertEqual("blu", tag)

        with self.assertRaisesRegex(ValueError, "group sizes must be identical"):
            ft_c._expand_group([[0], [1, 2, 3]])

    def test_expand_process_group(self):
        tag, rankset, stride = ft_c._expand_group(dist.group.WORLD)
        self.assertEqual(c10d._get_group_tag(dist.group.WORLD), tag)
        self.assertEqual([0, 1, 2, 3], rankset)
        self.assertEqual(4, stride)

        tag, rankset, stride = ft_c._expand_group(dist.group.WORLD, "bla")
        self.assertEqual("bla", tag)

        my_pg, others = new_subgroups(group_size=2)
        tag, rankset, stride = ft_c._expand_group(my_pg)
        self.assertEqual(c10d._get_group_tag(my_pg), tag)
        self.assertEqual(dist.get_process_group_ranks(my_pg), rankset)
        self.assertEqual(2, stride)

        my_pg = None
        for i in range(dist.get_world_size()):
            group = dist.new_group([i], pg_tag="my_pg")
            if i == dist.get_rank():
                my_pg = group
        tag, rankset, stride = ft_c._expand_group(my_pg)
        self.assertEqual("my_pg", tag)
        self.assertEqual([dist.get_rank()], rankset)
        self.assertEqual(1, stride)

        tag, rankset, stride = ft_c._expand_group(my_pg, "bla")
        self.assertEqual("bla", tag)

    def test_expand_device_mesh(self):
        mesh = dt.DeviceMesh("cpu", torch.arange(4))
        tag, rankset, stride = ft_c._expand_group(mesh)
        self.assertEqual(c10d._get_group_tag(mesh.get_dim_groups()[0]), tag)
        self.assertEqual([0, 1, 2, 3], rankset)
        self.assertEqual(4, stride)

        mesh = dt.DeviceMesh("cpu", torch.arange(4))
        tag, rankset, stride = ft_c._expand_group(mesh)
        self.assertEqual(c10d._get_group_tag(mesh.get_dim_groups()[0]), tag)
        self.assertEqual([0, 1, 2, 3], rankset)
        self.assertEqual(4, stride)

    def test_expand_device_mesh_tuple(self):
        mesh = dt.DeviceMesh("cpu", torch.arange(4).view(2, 2))
        tag, rankset, stride = ft_c._expand_group(mesh)
        self.assertEqual(c10d._get_group_tag(mesh.get_dim_groups()[0]), tag)
        self.assertEqual([0, 2, 1, 3], rankset)
        self.assertEqual(2, stride)

        tag, rankset, stride = ft_c._expand_group((mesh, 0))
        self.assertEqual(c10d._get_group_tag(mesh.get_dim_groups()[0]), tag)
        self.assertEqual([0, 2, 1, 3], rankset)
        self.assertEqual(2, stride)

        tag, rankset, stride = ft_c._expand_group((mesh, 1))
        self.assertEqual(c10d._get_group_tag(mesh.get_dim_groups()[1]), tag)
        self.assertEqual([0, 1, 2, 3], rankset)
        self.assertEqual(2, stride)

class TestPgTag(MultiThreadedTestCase):
    @property
    def world_size(self):
        return 4

    def setUp(self):
        super().setUp()
        self._spawn_threads()

    """
    The behavior we want is as follow:

    - rankset+tag will always result in the same PG.
    Do we enforce this by failing creation of new PGs or returning existing ones?
        Return existing one.

    - default tag gives existing behavior.
        This means we should create duplicates.
    - _expand_group on _default-tagged pg should always resolve to it
        This mean we can't depend on empty tag + rankset.
    """
    def test_pg_creation_with_tag(self):
        my_group, _ = new_subgroups(group_size=2, pg_tag="blu")
        my_group2, _ = new_subgroups(group_size=2, pg_tag="blu")
        self.assertEqual(my_group, my_group2)

        my_group3, _ = new_subgroups(group_size=2, pg_tag="blu2")
        self.assertNotEqual(my_group, my_group3)

        my_group4, _ = new_subgroups(group_size=2)
        self.assertNotEqual(my_group, my_group4)

        my_group5, _ = new_subgroups(group_size=2)
        self.assertNotEqual(my_group4, my_group5)

    def test_pg_lookup_roundtrip(self):
        pg_tag0, _ = new_subgroups(group_size=2, pg_tag="blu")
        pg_tag1, _ = new_subgroups(group_size=2, pg_tag="blu2")
        pg_notag0, _ = new_subgroups(group_size=2)
        pg_notag1, _ = new_subgroups(group_size=2)

        def roundtrip(pg):
            tag, rankset, _ = ft_c._expand_group(pg)
            return c10d._try_find_pg_by_ranks_and_tag(tag, rankset)

        self.assertEqual(pg_tag0, roundtrip(pg_tag0))
        self.assertEqual(pg_tag1, roundtrip(pg_tag1))
        self.assertEqual(pg_notag0, roundtrip(pg_notag0))
        self.assertEqual(pg_notag1, roundtrip(pg_notag1))

    def test_pg_lookup_with_tag(self):
        pg_tag0, _ = new_subgroups(group_size=2, pg_tag="blu")
        pg_tag1, _ = new_subgroups(group_size=2, pg_tag="bla")
        pg_notag0, _ = new_subgroups(group_size=2)

        def roundtrip(pg, pg_tag):
            tag, rankset, _ = ft_c._expand_group(pg, pg_tag)
            return c10d._try_find_pg_by_ranks_and_tag(tag, rankset)

        self.assertEqual(pg_tag0, roundtrip(pg_tag1, "blu"))
        self.assertEqual(pg_tag0, roundtrip(pg_notag0, "blu"))
        # Cannot erase the tag of a PG
        self.assertEqual(pg_tag0, roundtrip(pg_tag0, ""))

    def test_find_or_create_pg(self):
        pg = c10d._find_or_create_pg_by_ranks_and_tag("blu", [0, 1, 2, 3], 2)
        pg_tag0, _ = new_subgroups(group_size=2, pg_tag="blu")
        self.assertEqual(pg, pg_tag0)

class TestTraceableCollectives(MultiThreadedTestCase):
    @property
    def world_size(self):
        return 4

    def setUp(self):
        super().setUp()
        self._spawn_threads()

    def test_all_reduce_eager(self):
        tensor = torch.ones([4])
        mesh = dt.DeviceMesh("cpu", torch.arange(4))

        res = ft_c.all_reduce(tensor, "sum", mesh)
        self.assertEqual(res, torch.tensor([4, 4, 4, 4], dtype=torch.float))

        mesh = dt.DeviceMesh("cpu", torch.arange(4).view(2, 2))
        res2 = ft_c.all_reduce(tensor, "sum", (mesh, 1))
        self.assertEqual(res2, torch.tensor([2, 2, 2, 2], dtype=torch.float))

if __name__ == "__main__":
    run_tests()
