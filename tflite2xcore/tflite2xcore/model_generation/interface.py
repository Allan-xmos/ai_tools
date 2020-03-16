# Copyright (c) 2018-2019, XMOS Ltd, All rights reserved
import logging
import pathlib
from tflite2xcore.model_generation import utils
import tensorflow as tf
import numpy as np
from abc import ABC, abstractmethod
import tflite2xcore.converter as xcore_conv
from tflite2xcore import tflite_visualize
from tflite2xcore.serialization.flatbuffers_io import serialize_model, deserialize_model
from tflite2xcore.xcore_model import TensorType


class Model(ABC):
    def __init__(self, name, path):
        '''
        Initialization function of the class Model. Parameters needed are:
        \t- name     (string): Name of the model and model directory name
        \t- path       (Path): Working directory where everything is stored
        Other properties derived:
        \t- core_model(Model): The main model from which others derive
        \t- models     (dict): To store converted model paths
        \t\t- keys: 'model_float', 'model_quant', 'model_stripped', 'model_xcore'
        \t- buffers    (dict): To store converted serialized models
        \t\t- keys: same as models
        \t- model_dir  (Path): parent directory of model paths
        \t- data_dir   (Path): directory where data is stored
        \t- data       (dict): various data arrays
        \t\t- keys: 'quant', 'export'
        \t- converters (dict): To store all TFLite converter objects
        \t\t- keys: 'model_float', 'model_quant'
        '''
        self.name = name
        self.core_model = None
        self.models = {}
        self.buffers = {}
        self.data = {}
        self.converters = {}

        if isinstance(path, pathlib.Path):
            self._path = path
        elif isinstance(path, str):
            self._path = pathlib.Path(path)
        else:
            raise TypeError(f"Expected path of type str or pathlib.Path, got {type(path)}")

        self._path.mkdir(parents=True, exist_ok=True)
        self.data_dir = self._path / 'test_data'
        self.data_dir.mkdir(exist_ok=True)
        self.models_dir = self._path / 'models'
        self.models_dir.mkdir(exist_ok=True)

    @abstractmethod
    def build(self):
        '''
        Here should be the model definition to be built,
        compiled and summarized. The model should be stored in self.core_model.
        '''
        pass

    @abstractmethod
    def _save_core_model(self):
        raise NotImplementedError("This function should not be called")

    def _save_training_data(self):
        keys = self.data.keys()
        if keys:
            logging.info(f"Saving the following data keys: {keys}")
            np.savez(self.data_dir / 'data', **self.data)

    def save_core_model(self):
        '''
        Function to store training data and original model files in the
        corresponding format.
        '''
        self._save_training_data()
        self.models['core'] = self.models_dir / 'model.h5'
        logging.info(f"Saving {self.__class__.__name__} to {self.models['core']}")
        self._save_core_model()

    @abstractmethod
    def _load_core_model(self):
        raise NotImplementedError("This function should not be called")

    def _load_training_data(self):
        data_path = self.data_dir / 'data.npz'
        logging.info(f"Loading data from {data_path}")
        self.data = dict(np.load(data_path))

    def load_core_model(self):
        '''
        If we don't want to build our model from scratch and
        we have it stored somewhere, we can load it with this function.
        '''
        self.models['core'] = self.models_dir / 'model.h5'
        try:
            self._load_training_data()
            logging.info(f"Loading {self.__class__.__name__} from {self.models['core']}")
            self._load_core_model()
        except FileNotFoundError as e:
            raise FileNotFoundError(
                f"Model file not found (Hint: use the --train_model flag)") from e

    @abstractmethod
    def prep_data(self):
        '''
        To prepare or download the training and test data.
        Should return a dictionary:
        {'x_train':xt, 'y_train':yt, 'x_test':xtt, 'y_test':ytt}
        '''
        pass

    @abstractmethod
    def train(self):
        '''
        Fit with hyperparams and if we want to save
        original model and training data,
        that should be done here.
        '''
        pass

    @abstractmethod
    def gen_test_data(self):
        '''
        Select the test data examples for storing
        along with the converted models.
        Must fill the data dictionary with an entries 'export' and 'quant'
        '''
        pass

    @abstractmethod
    def populate_converters(self):
        assert self.core_model, ("core model has not been initialized "
                                 "(Hint: run Model.build first)")
        assert 'quant' in self.data, ("representative dataset has not been prepared"
                                      "(Hint: run Model.gen_test_data first)")

    def convert_to_float(self, **converter_args):
        self._convert_from_tflite_converter('model_float')

    def convert_to_quant(self, **converter_args):
        self._convert_from_tflite_converter('model_quant')

    def convert_to_stripped(self, **converter_args):
        assert 'model_quant' in self.buffers
        logging.info(f"Converting model_quant...")
        model = deserialize_model(self.buffers['model_quant'])
        xcore_conv.strip_model(model, **converter_args)
        self.models['model_stripped'] = self.models_dir / "model_stripped.tflite"
        self.buffers['model_stripped'] = serialize_model(model)

    def convert_to_xcore(self, *, source='model_quant', **converter_args):
        assert source in ['model_quant', 'model_stripped']
        assert source in self.buffers
        logging.info(f"Converting {source}...")
        model = deserialize_model(self.buffers[source])
        xcore_conv.optimize_for_xcore(model, **converter_args)
        self.models['model_xcore'] = self.models_dir / 'model_xcore.tflite'
        self.buffers['model_xcore'] = serialize_model(model)

    def _convert_from_tflite_converter(self, model_key):
        assert model_key in self.converters
        logging.info(f"Converting {model_key}...")
        self.models[model_key] = self.models_dir / f"{model_key}.tflite"
        self.buffers[model_key] = self.converters[model_key].convert()

    def _save_buffer(self, model_key):
        size = self.models[model_key].write_bytes(self.buffers[model_key])
        logging.info(f"{self.models[model_key]} size: {size/1024:.0f} KB")

    def _save_visualization(self, model_key):
        assert model_key in self.models, 'Model needs to exist to prepare visualization.'
        model_html = self.models[model_key].with_suffix('.html')
        tflite_visualize.main(self.models[model_key], model_html)
        logging.info(f"{model_key} visualization saved to {model_html}")

    def _save_data_dict(self, data, *, base_file_name):
        # TODO: this should probably be a util
        # save test data in numpy format
        test_data_dir = self.data_dir / base_file_name
        test_data_dir.mkdir(exist_ok=True, parents=True)
        np.savez(test_data_dir / f"{base_file_name}.npz", **data)

        # save individual binary files for easier low level access
        for key, test_set in data.items():
            for j, arr in enumerate(test_set):
                with open(test_data_dir / f"test_{j}.{key[0]}", 'wb') as f:
                    f.write(arr.flatten().tostring())

        logging.info(f"test examples for {base_file_name} saved to {test_data_dir}")

    def _save_data_for_canonical_model(self, model_key):
        interpreter = tf.lite.Interpreter(model_content=self.buffers[model_key])

        # extract labels for the test examples
        logging.info(f"Extracting examples for {model_key}...")
        x_test = self.data['export']
        data = {'x_test': x_test,
                'y_test': utils.apply_interpreter_to_examples(interpreter, x_test)}

        self._save_data_dict(data, base_file_name=model_key)

    def save_float_data(self):
        assert 'model_float' in self.buffers
        self._save_data_for_canonical_model('model_float')

    def save_quant_data(self):
        assert 'model_quant' in self.buffers
        self._save_data_for_canonical_model('model_quant')

    def save_stripped_data(self):
        assert 'model_stripped' in self.buffers

        model = deserialize_model(self.buffers['model_stripped'])
        output_quant = model.subgraphs[0].outputs[0].quantization
        input_quant = model.subgraphs[0].inputs[0].quantization
        xcore_conv.add_float_input_output(model)

        interpreter = tf.lite.Interpreter(model_content=serialize_model(model))

        # extract and quantize reference labels for the test examples
        logging.info("Extracting examples for model_stripped...")
        x_test = utils.quantize(self.data['export'],
                                input_quant['scale'][0],
                                input_quant['zero_point'][0])
        y_test = utils.apply_interpreter_to_examples(interpreter, self.data['export'])
        # The next line breaks in FunctionModels & Keras without output dimension
        y_test = [
            utils.quantize(y, output_quant['scale'][0], output_quant['zero_point'][0])
            for y in y_test
        ]
        data = {'x_test': x_test,
                'y_test': np.vstack(y_test)}

        self._save_data_dict(data, base_file_name='model_stripped')

    def save_xcore_data(self):
        assert 'model_xcore' in self.buffers

        model = deserialize_model(self.buffers['model_xcore'])
        input_tensor = model.subgraphs[0].inputs[0]
        input_quant = input_tensor.quantization

        if input_tensor.type != TensorType.INT8:
            raise NotImplementedError(f"input tensor type {input_tensor.type} "
                                      "not supported in save_xcore_data")

        # quantize test data
        x_test = utils.quantize(self.data['export'],
                                input_quant['scale'][0],
                                input_quant['zero_point'][0])

        # we pad tensor dimensions other than the first (i.e. batch)
        assert len(input_tensor.shape) == len(x_test.shape)
        pad_width = [(0, input_tensor.shape[j] - x_test.shape[j] if j > 0 else 0)
                     for j in range(len(x_test.shape))]
        x_test = np.pad(x_test, pad_width)

        # save data
        self._save_data_dict({'x_test': x_test}, base_file_name='model_xcore')

    def _convert_to(self, model_key, **converter_args):
        converters = {
            'model_float': self.convert_to_float,
            'model_quant': self.convert_to_quant,
            'model_stripped': self.convert_to_stripped,
            'model_xcore': self.convert_to_xcore
        }
        converters[model_key](**converter_args)

    def _save_data_for(self, model_key):
        savers = {
            'model_float': self.save_float_data,
            'model_quant': self.save_quant_data,
            'model_stripped': self.save_stripped_data,
            'model_xcore': self.save_xcore_data
        }
        savers[model_key]()

    def convert_and_save(self, *,
                         visualize=True, save_buffers=True, xcore_num_threads=None):
        self.gen_test_data()
        self.populate_converters()
        for model_key in ['model_float', 'model_quant', 'model_stripped', 'model_xcore']:
            if model_key is 'model_xcore':
                self._convert_to(model_key, num_threads=xcore_num_threads)
            else:
                self._convert_to(model_key)
            if save_buffers:
                self._save_buffer(model_key)
            if visualize:
                self._save_visualization(model_key)
            self._save_data_for(model_key)


class KerasModel(Model):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.input_init = None

    def _prep_backend(self):
        tf.keras.backend.clear_session()
        utils.set_all_seeds()

    @property
    def input_shape(self):
        return self.core_model.input_shape[1:]

    @property
    def output_shape(self):
        return self.core_model.output_shape[1:]

    def train(self, save_history=True, **kwargs):
        assert self.data, ("training dataset has not been prepared"
                           "(Hint: run Model.prep_data first)")
        self.history = self.core_model.fit(
            self.data['x_train'], self.data['y_train'],
            validation_data=(self.data['x_test'], self.data['y_test']),
            **kwargs
        )
        if save_history:
            self.save_training_history()

    def save_training_history(self):
        with utils.LoggingContext(logging.getLogger(), logging.INFO):
            utils.plot_history(self.history,
                               title=f"{self.name} metrics",
                               path=self.models_dir / 'training_history.png')

    def _save_core_model(self):
        self.core_model.save(self.models['core'])

    def _load_core_model(self):
        self.core_model = tf.keras.models.load_model(self.models['core'])

    def populate_converters(self):
        super().populate_converters()
        self.converters['model_float'] = tf.lite.TFLiteConverter.from_keras_model(
            self.core_model)
        self.converters['model_quant'] = tf.lite.TFLiteConverter.from_keras_model(
            self.core_model)
        utils.quantize_converter(
            self.converters['model_quant'], self.data['quant'])


class KerasClassifier(KerasModel):
    def __init__(self, *args, opt_classifier=False, **kwargs):
        super().__init__(*args, **kwargs)
        self._opt_classifier = opt_classifier

    def convert_to_xcore(self, **converter_args):
        converter_args.setdefault('is_classifier', self._opt_classifier)
        super().convert_to_xcore(**converter_args)


class FunctionModel(Model):

    def __init__(self, name, path):
        super().__init__(name, path)
        self.loaded = False

    def _save_core_model(self):
        tf.saved_model.save(self.core_model, str(self.models['core']),
                            signatures=self.concrete_function)

    def _load_core_model(self):
        self.core_model = tf.saved_model.load(str(self.models['core']))

    @property
    @abstractmethod
    def concrete_function(self):
        pass

    def populate_converters(self):
        super().populate_converters()
        self.converters['model_float'] = tf.lite.TFLiteConverter.from_concrete_functions(
            [self.concrete_function])
        self.converters['model_quant'] = tf.lite.TFLiteConverter.from_concrete_functions(
            [self.concrete_function])
        utils.quantize_converter(
            self.converters['model_quant'], self.data['quant'])
