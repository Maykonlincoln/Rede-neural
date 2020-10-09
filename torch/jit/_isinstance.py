from typing import List, Dict, Tuple, Union


def get_origin(the_type):
    return getattr(the_type, "__origin__", None)


def get_args(the_type):
    return getattr(the_type, "__args__", None)


def generics_checker(the_obj, the_type):
    origin_type = get_origin(the_type)
    if origin_type is None:
        pass
    elif origin_type is list or origin_type is List:
        if isinstance(the_obj, list):
            for el in the_obj:
                # check if nested generics, ex: List[List[str]]
                arg_type = get_args(the_type)[0]
                arg_origin = get_origin(arg_type)
                if arg_origin:  # processes nested generics, ex: List[List[str]]
                    if not generics_checker(el, arg_type):
                        return False
                elif not isinstance(el, arg_type):
                    return False
        else:
            return False
    elif origin_type is dict or origin_type is Dict:
        if isinstance(the_obj, dict):
            key_type = get_args(the_type)[0]
            val_type = get_args(the_type)[1]
            for key, val in the_obj.items():
                # check if keys are of right type
                if not isinstance(key, key_type):
                    return False
                val_origin = get_origin(val_type)
                if val_origin:
                    if not generics_checker(val, val_type):
                        return False
                elif not isinstance(val, val_type):
                    return False
        else:
            return False
    elif origin_type is Union:  # TODO actually handles Optional Case
        if the_obj is None:  # check before recursion because None is always fine
            return True
        optional_type = get_args(the_type)[0]
        optional_origin = get_origin(optional_type)
        if optional_origin:
            return generics_checker(the_obj, optional_type)
        elif isinstance(the_obj, optional_type):
            return True
        else:
            return False
    elif origin_type is tuple or Tuple:
        if isinstance(the_obj, tuple):
            arg_types = get_args(the_type)
            if len(the_obj) != len(arg_types):
                return False  # TODO actually figure out what should happen here
            for el, el_type in zip(the_obj, arg_types):
                el_origin = get_origin(el_type)
                if el_origin:
                    if not generics_checker(el, el_type):
                        return False
                elif not isinstance(el, el_type):
                    return False
        else:
            return False
    return True


def _isinstance(the_obj, the_type) -> bool:
    origin_type = get_origin(the_type)
    if origin_type:
        return generics_checker(the_obj, the_type)
    # handle non-generics
    return isinstance(the_obj, the_type)
