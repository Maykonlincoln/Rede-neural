# Mypy will not try inferring the types of any 3rd party libraries installed.
# mypy: ignore-errors

import io
import os
from contextlib import contextmanager
from typing import Generator, Optional, Union

import fsspec
from fsspec import AbstractFileSystem
from fsspec.core import url_to_fs

from torch.distributed.checkpoint.filesystem import (
    FileSystemBase,
    FileSystemReader,
    FileSystemWriter,
)

__all__ = [
    "FsspecWriter",
    "FsspecReader",
]


class FileSystem(FileSystemBase):
    def __init__(self) -> None:
        self.fs: Optional[AbstractFileSystem] = None

    @contextmanager
    def create_stream(
        self, path: Union[str, os.PathLike], mode: str
    ) -> Generator[io.IOBase, None, None]:
        assert self.fs is not None
        with self.fs.transaction:
            with fsspec.open(str(path), mode) as stream:
                yield stream

    def concat_path(
        self, path: Union[str, os.PathLike], suffix: str
    ) -> Union[str, os.PathLike]:
        return os.path.join(path, suffix)

    def init_path(self, path: Union[str, os.PathLike]) -> Union[str, os.PathLike]:
        self.fs, _ = url_to_fs(path)
        return path

    def rename(
        self, path: Union[str, os.PathLike], new_path: Union[str, os.PathLike]
    ) -> None:
        self.fs.rename(path, new_path)

    def mkdir(self, path: [str, os.PathLike]) -> None:
        self.fs.makedirs(path, exist_ok=True)


class FsspecWriter(FileSystemWriter):
    """
    Basic implementation of StorageWriter using FFspec.

    This implementation makes the following assumptions and simplifications:

    * The checkpoint path is an empty or non-existing directory.
    * File creation is atomic

    The checkpoint consist of one file per write request plus
    a `.metadata` file with the serialized metadata.

    """

    def __init__(
        self,
        path: Union[str, os.PathLike],
        single_file_per_rank: bool = True,
        sync_files: bool = True,
        thread_count: int = 1,
        per_thread_copy_ahead: int = 10_000_000,
    ) -> None:
        """
        Initialize the writer pointing to `path`.

        Args:
            path: directory where the checkpoint will be written to.
            single_file_per_rank: Produce one file per rank instead of one file per tensor/blob. Default to True.
            sync_files : force files to be synced to permanent storage. Default to True.
            thread_count: Number of IO threads to use to write. Default to 1.
            per_thread_copy_ahead: How many bytes to copy from the GPU ahead of saving then. Default 10Mb.

        N. B. If sync_files is disabled, there's no guarantee that the checkpoint will be consistent in the case of a failure.
        """
        super().__init__(
            path, single_file_per_rank, sync_files, thread_count, per_thread_copy_ahead
        )
        self.fs = FileSystem()
        self.path = self.fs.init_path(path)


class FsspecReader(FileSystemReader):
    def __init__(self, path: Union[str, os.PathLike]) -> None:
        super().__init__(path)
        self.fs = FileSystem()
        self.path = self.fs.init_path(path)
