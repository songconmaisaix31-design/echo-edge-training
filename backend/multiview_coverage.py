from typing import Literal


View = Literal["A4C", "A2C", "A3C", "A5C", "PLAX", "PSAX", "subcostal", "suprasternal"]


def view_coverage(required, confirmed):
    seen = set(confirmed)
    return {view: "covered" if view in seen else "missing" for view in required}
